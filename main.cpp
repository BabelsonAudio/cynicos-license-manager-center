#include <iostream>
#include <cstdio>
#include "raylib.h"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <regex>
#include <alsa/asoundlib.h>
#include <mutex>       // 👈 ADDED for thread safety
#include <fcntl.h>     // For open()
#include <unistd.h>    // For read() and close()
#include "NoiseRemoval.h" // 1. Pull in your static library's header!

// --- CORE MIXER DATA ---

std::vector<double> volumes(9, 40.0); // 9 channels starting at 40%
int active_ch = 0;
const char* tags[] = {"MAST", "FL", "FR", "CNTR", "SUB", "SL", "SR", "RL", "RR"};

// --- SPATIAL PUCK GLOBALS ---
Vector2 puck_pos = {160, 270}; // Center of the grid area
bool puck_grabbed = false;

// --- TOUCH/MOUSE INTERACTION STATE ---
int grabbed_fader = -1; // -1 means no fader is being dragged

// --- BLUETOOTH GLOBALS ---
std::string current_title = "No Track Playing";
std::string current_artist = "Unknown Device";
std::mutex metadata_mutex; // 👈 ADDED to protect strings from crashing the GUI

std::atomic<float> real_vu_peak(0.0f); 
std::atomic<bool> system_running(true); 
std::atomic<bool> bt_ready_flag(false);

// --- BLUETOOTH BACKGROUND FETCHER ---
std::string query_bluetooth(const std::string& type) {
    std::string command = "dbus-send --system --type=method_call --print-reply "
                          "--dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects "
                          "| grep -o '/org/bluez/hci0/dev_[^/]*/player[0-9]*' | head -n 1";

    std::array<char, 128> buffer;
    std::string player_path = "";
    
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe) {
        if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            player_path = buffer.data();
        }
        pclose(pipe);
    }
    
    if (!player_path.empty() && player_path.back() == '\n') player_path.pop_back();
    if (player_path.empty()) return "No Track Playing";

    // Validate player_path to prevent command injection: only accept well-known BlueZ object paths
    static const std::regex valid_player_path(
        "^/org/bluez/hci[0-9]+/dev_([0-9A-F]{2}_){5}[0-9A-F]{2}/player[0-9]+$"
    );
    if (!std::regex_match(player_path, valid_player_path)) return "No Track Playing";

    command = "dbus-send --system --type=method_call --print-reply "
              "--dest=org.bluez " + player_path + " "
              "org.freedesktop.DBus.Properties.Get string:org.bluez.MediaPlayer1 string:Track "
              "| grep -A 1 '\"" + type + "\"' | tail -n 1 | cut -d'\"' -f2";
              
    std::string result = "";
    pipe = popen(command.c_str(), "r");
    if (!pipe) return "Not Playing";
    
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
    }
    pclose(pipe);
    
    if (!result.empty() && result.back() == '\n') result.pop_back();
    if (result.empty() || result.find("Error") != std::string::npos) {
        return "No Track Playing";
    }
    
    return result;
}

void fetch_metadata_thread() {
    while (system_running) {
        bt_ready_flag.store(system("pgrep bluealsa > /dev/null") == 0);
    
        std::string t = query_bluetooth("Title");
        std::string a = query_bluetooth("Artist");
        
        // 👈 LOCK MUTEX before writing to shared strings
        std::lock_guard<std::mutex> lock(metadata_mutex);
        if (t != "Not Playing") current_title = t;
        if (a != "Not Playing") current_artist = a;
        
        std::this_thread::sleep_for(std::chrono::seconds(2)); 
    }
}

// --- REAL VU METER CAPTURE THREAD ---
void capture_bluealsa_vu_thread() {
    int pipe_fd = open("/tmp/bluealsa_vu_pipe", O_RDONLY | O_NONBLOCK);
    const int buffer_size = 64; 
    std::vector<short> buffer(buffer_size);

    while (system_running) {
        ssize_t bytes_read = -1;
        bool data_found = false;
        short peak_sample = 0;

        if (pipe_fd >= 0) {
            while ((bytes_read = read(pipe_fd, buffer.data(), buffer_size * sizeof(short))) > 0) {
                data_found = true;
                int read_shorts = bytes_read / sizeof(short);
                
                for (int i = 0; i < read_shorts; i++) {
                    short abs_val = std::abs(buffer[i]);
                    if (abs_val > peak_sample) peak_sample = abs_val;
                }
            }
        }
        
        if (data_found && peak_sample > 0) {
            float instant_vu = (float)peak_sample / 32767.0f;
            instant_vu = sqrtf(instant_vu);
            float previous_vu = real_vu_peak.load();
            
            if (instant_vu > previous_vu) {
                real_vu_peak.store(instant_vu);
            } else {
                float smooth_decay = previous_vu * 0.50f; 
                if (smooth_decay < 0.01f) smooth_decay = 0.0f;
                real_vu_peak.store(smooth_decay);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15)); // was 1
        } 
        else {
            // No signal: decay the VU peak smoothly to zero so meters stay still
            float previous_vu = real_vu_peak.load();
            float smooth_decay = previous_vu * 0.50f;
            if (smooth_decay < 0.01f) smooth_decay = 0.0f;
            real_vu_peak.store(smooth_decay);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    if (pipe_fd >= 0) close(pipe_fd);
}

// --- WORKING BLUETOOTH VOLUME FUNCTION ---
void set_bluetooth_volume(long volume_percent) {
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    const char *card = "bluealsa"; 
    const char *selem_name = "Thomas iPhone A2DP"; 

    if (snd_mixer_open(&handle, 0) < 0) return;
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    if (!elem) {
        snd_mixer_close(handle);
        return;
    }

    long min, max;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    long volume_value = (volume_percent * (max - min) / 100) + min;
    
    snd_mixer_selem_set_playback_volume_all(elem, volume_value);
    snd_mixer_selem_set_capture_volume_all(elem, volume_value);

    snd_mixer_close(handle);
}

// --- MASTER VOLUME LOGIC ---
void adjust_master(double delta) {
    double old_m = volumes[0];
    volumes[0] += delta;
    if (volumes[0] > 100) volumes[0] = 100;
    if (volumes[0] < 0) volumes[0] = 0;
    
    if (old_m > 0) {
        double ratio = volumes[0] / old_m;
        for (int i = 1; i < 9; i++) {
            volumes[i] *= ratio;
            if (volumes[i] > 100) volumes[i] = 100;
            if (volumes[i] < 0) volumes[i] = 0;
        }
    }
    set_bluetooth_volume((long)volumes[0]);
}

// --- EQ CHANNEL APPLY LOGIC ---
void apply_eq_to_channels(float bass, float mid, float treble) {
    volumes[4] = volumes[0] * (bass * 1.5f); 
    volumes[3] = volumes[0] * (mid * 1.2f);
    volumes[5] = volumes[0] * mid;
    volumes[6] = volumes[0] * mid;
    volumes[1] = volumes[0] * (treble * 1.1f);
    volumes[2] = volumes[0] * (treble * 1.1f);
    volumes[7] = volumes[0] * treble;
    volumes[8] = volumes[0] * treble;

    for(int i = 1; i < 9; i++) {
        if (volumes[i] > 100.0) volumes[i] = 100.0;
        if (volumes[i] < 0.0) volumes[i] = 0.0;
    }
}

// Fixed position for helper function (out of main)
void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments, float lineThick, Color color) {
    // Falls back to standard raylib lines since ES2/DRM doesn't love heavy geometry
    // Pass lineThick as the 4th parameter, and color as the 5th
    DrawRectangleRoundedLines(rec, roundness, segments, lineThick, color);
}

int main() {
    std::cout << "Cynicos Aura Core: Initializing EV Audio Control Interface..." << std::endl;
    InitWindow(1280, 720, "CYNICOS AURA CORE");

    if (!IsWindowReady()) return 1;
    
    // --- handle this automatically ---
    // Clean up any lingering audio locks before starting our window
    system("sudo killall bluealsa-aplay 2>/dev/null");
    system("sudo systemctl restart bt-audio-router.service");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Give it a second to settle
    // ---------------------
    
   // 1. Put all your required letters in a single clean UTF-8 string
    const char *chars = u8"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;':\",./<>? ÇçĞğİıŞşÖöÜü";

    // 2. Let Raylib automatically extract the codepoints from the string
    int codepointCount = 0;
    int *codepoints = LoadCodepoints(chars, &codepointCount);

    // 3. Sort them so LoadFontEx doesn't fail its internal binary search
    std::sort(codepoints, codepoints + codepointCount);

    // 4. Load the font using the auto-generated codepoint list
    Font customFont = LoadFontEx("resources/fonts/NotoSans-Regular.ttf", 32, codepoints, codepointCount);

    // 5. Free the temporary codepoints list memory
    UnloadCodepoints(codepoints);

    ToggleFullscreen();
    SetTargetFPS(60);

    std::thread meta_thread(fetch_metadata_thread);
    std::thread audio_thread(capture_bluealsa_vu_thread);

    // ============================================================================
    // AURA DENOISER SETUP
    // ============================================================================
    std::cout << "Initializing Aura Denoiser..." << std::endl;
    
    // Using heap allocation to prevent stack overflow crashes
    auto denoiser = std::make_unique<NoiseRemoval>();
    denoiser->prepare(44100.0);
    //denoiser->prepare(16000.0);
    // ============================================================================

    static float eq_vals[3] = {0.5f, 0.4f, 0.6f}; // Loaded before loop to prevent resets

    while (!WindowShouldClose()) {
        Vector2 mouse = GetMousePosition();

        // --- INPUT & TOUCH INTERACTION ---
        if (IsKeyPressed(KEY_RIGHT) && active_ch < 8) active_ch++;
        if (IsKeyPressed(KEY_LEFT) && active_ch > 0) active_ch--;

        if (IsKeyDown(KEY_UP)) {
            if (active_ch == 0) adjust_master(0.5);
            else if (volumes[active_ch] < 100) volumes[active_ch] += 0.5;
        }
        if (IsKeyDown(KEY_DOWN)) {
            if (active_ch == 0) adjust_master(-0.5);
            else if (volumes[active_ch] > 0) volumes[active_ch] -= 0.5; 
        }

        // --- PRESETS CLICKS ---
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointRec(mouse, (Rectangle){60, 410, 80, 20})) {
                volumes = {70.0, 90.0, 30.0, 40.0, 50.0, 20.0, 20.0, 10.0, 10.0};
                set_bluetooth_volume(70);
            }
            if (CheckCollisionPointRec(mouse, (Rectangle){150, 410, 80, 20})) {
                volumes = {60.0, 40.0, 40.0, 90.0, 95.0, 50.0, 50.0, 30.0, 30.0};
                set_bluetooth_volume(60);
            }
            if (CheckCollisionPointRec(mouse, (Rectangle){240, 410, 80, 20})) {
                volumes = {100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0};
                set_bluetooth_volume(100);
            }
        }

        // --- 1. TOUCH & DRAG FADERS LOGIC ---
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            for (int i = 0; i < 9; i++) {
                int x = 360 + (i * 95); 
                float height = (volumes[i] / 100.0f) * 160;
                
                // --- FINGER-FRIENDLY TOUCH TARGET ---
                // We keep the drawing thin, but give the finger a massive 60x40 target!
                float center_x = (float)x + 27;
                float center_y = 360.0f - height;

                Rectangle handle_box = {
                    center_x - 30.0f,  // 30px to the left
                    center_y - 20.0f,  // 20px up
                    60.0f,             // 60px total width!
                    40.0f              // 40px total height!
                };
                
                if (CheckCollisionPointRec(mouse, handle_box)) {
                    grabbed_fader = i;
                    active_ch = i;
                }
            }
        }

        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && grabbed_fader != -1) {
            float new_y = mouse.y;
            if (new_y < 200) new_y = 200;
            if (new_y > 360) new_y = 360;
            
            float percentage = ((360 - new_y) / 160.0f) * 100.0f;
            
            if (grabbed_fader == 0) {
                adjust_master(percentage - volumes[0]);
            } else {
                volumes[grabbed_fader] = percentage;
            }
        }

        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            grabbed_fader = -1;
        }

        // --- 2. THE SPATIAL PUCK LOGIC ---
        Rectangle puck_bounds = {60, 160, 200, 220};
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointCircle(mouse, puck_pos, 12)) puck_grabbed = true;
        }
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && puck_grabbed) {
            puck_pos = mouse;
            if (puck_pos.x < puck_bounds.x) puck_pos.x = puck_bounds.x;
            if (puck_pos.x > puck_bounds.x + puck_bounds.width) puck_pos.x = puck_bounds.x + puck_bounds.width;
            if (puck_pos.y < puck_bounds.y) puck_pos.y = puck_bounds.y;
            if (puck_pos.y > puck_bounds.y + puck_bounds.height) puck_pos.y = puck_bounds.y + puck_bounds.height;
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) puck_grabbed = false;

        apply_eq_to_channels(eq_vals[0], eq_vals[1], eq_vals[2]);

        // --- DRAWING ---
        BeginDrawing();
        ClearBackground({10, 11, 14, 255}); 

        DrawText("CYNICOS AURA", 40, 30, 24, WHITE);
        
        // REPLACED system() CALL WITH ATOMIC LOAD FOR STABILITY
        bool bt_ready = bt_ready_flag.load();
        
        DrawCircle(1140, 42, 4, bt_ready ? (Color){0, 255, 170, 255} : (Color){100, 100, 100, 255});
        DrawText(bt_ready ? "CONNECTED" : "DISCONNECTED", 1155, 35, 12, (Color){150, 150, 160, 255});

        DrawRectangleRounded((Rectangle){40, 100, 1200, 340}, 0.05, 10, (Color){18, 19, 23, 200});
        DrawText("SPATIAL AUDIO FIELD", 60, 120, 14, (Color){100, 100, 110, 255});

        // --- DRAW PRESET BUTTONS ---
        DrawRectangleRounded((Rectangle){60, 410, 80, 20}, 0.2, 5, (Color){30, 32, 38, 255});
        DrawText("DRIVER", 80, 414, 11, WHITE);
        
        DrawRectangleRounded((Rectangle){150, 410, 80, 20}, 0.2, 5, (Color){30, 32, 38, 255});
        DrawText("MOVIE", 172, 414, 11, WHITE);
        
        DrawRectangleRounded((Rectangle){240, 410, 80, 20}, 0.2, 5, (Color){30, 32, 38, 255});
        DrawText("PARTY", 262, 414, 11, WHITE);

        // Draw Car Grid Area
        DrawRectangleRec(puck_bounds, (Color){25, 26, 31, 255});
        DrawRectangleLinesEx(puck_bounds, 1, (Color){40, 42, 50, 255});
        
        // Simple car body wireframe
        DrawRectangleLines(100, 200, 120, 140, (Color){60, 65, 75, 255});
        DrawCircleLines(120, 230, 10, (Color){60, 65, 75, 255}); // Steering wheel
        
        // Draggable puck
        DrawCircleV(puck_pos, 10, (Color){0, 255, 210, 255});
        DrawCircleLinesV(puck_pos, 15, (Color){0, 255, 210, 100});

       float current_signal_level = real_vu_peak.load();

        // 9 Channels - Lucid Motors "Minimalist Luxury" Style
        for (int i = 0; i < 9; i++) {
            int x = 360 + (i * 95); 
            float height = (volumes[i] / 100.0f) * 160.0f;
            
            // 🔓 Bypassed bt_ready so we can see the data
            float sig_bounce = current_signal_level * 160.0f; 
            if (sig_bounce > 160.0f) sig_bounce = 160.0f; 

            // 1. Lucid Background Track: Extremely thin 2px line
            DrawRectangle(x + 26, 200, 2, 160, (Color){35, 38, 46, 255});
            
            // 2. Minimalist Peak Indicator: A flat 1px horizontal dash
            Color peak_color = (current_signal_level > 0.93f) ? (Color){255, 70, 70, 255} : (Color){60, 65, 75, 255};
            DrawRectangle(x + 17, 185, 20, 1, peak_color);

            // 3. Lucid Signal Fill (VU Level): Widened to 6px so it's visible!
            if (sig_bounce > 2.0f) {
                DrawRectangle(x + 24, 360 - sig_bounce, 6, sig_bounce, (Color){0, 255, 210, 200});
            }

            // 4. NEW RECTANGLE FADER HANDLE (Lucid Style)
            int rect_w = 40;
            int rect_h = 6;
            int rect_x = (x + 27) - (rect_w / 2);
            int rect_y = (360 - height) - (rect_h / 2); // 👈 Fixed scope!

            Color knobColor = (i == active_ch) ? WHITE : (Color){110, 115, 125, 255};
            
            // Draw the flat rectangle handle
            DrawRectangleRounded((Rectangle){(float)rect_x, (float)rect_y, (float)rect_w, (float)rect_h}, 0.3f, 4, knobColor);
            
            // Subtle premium horizontal accent line if active
            if (i == active_ch) {
                DrawRectangle(rect_x + 5, rect_y + 2, rect_w - 10, 2, (Color){0, 255, 210, 255});
            }

            // 5. Typography: Clean and dim unless active
            DrawText(tags[i], x + 12, 380, 11, (i == active_ch) ? WHITE : (Color){100, 102, 113, 255});
            DrawText(TextFormat("%d%%", (int)volumes[i]), x + 13, 155, 11, (i == active_ch) ? (Color){0, 255, 210, 255} : (Color){70, 72, 83, 255});
        }

        // --- FIX: DECODE UTF-8 FOR TURKISH LETTERS ---

// 1. Draw the Title
// =========================================
// 🎵 TITLE
// =========================================
int codepointCountTitle = 0;
int *codepointsTitle = LoadCodepoints(current_title.c_str(), &codepointCountTitle);

Vector2 titlePos = {150, 580};
float titleSize = 20.0f;
float titleScale = titleSize / customFont.baseSize;

for (int i = 0; i < codepointCountTitle; i++) 
{
    int cp = codepointsTitle[i];
    int index = GetGlyphIndex(customFont, cp);

    // ✨ subtle shadow
    DrawTextCodepoint(customFont, cp, {titlePos.x + 1.5f, titlePos.y + 1.5f}, titleSize, (Color){0,0,0,120});

    // main glyph
    DrawTextCodepoint(customFont, cp, titlePos, titleSize, WHITE);

    // safer advance
    float advance = (customFont.glyphs[index].advanceX > 0) ?
        (float)customFont.glyphs[index].advanceX :
        (float)customFont.recs[index].width;

    titlePos.x += (advance * titleScale) + 1.5f; // ✨ nicer spacing
}
UnloadCodepoints(codepointsTitle);


// =========================================
// 🎤 ARTIST
// =========================================
int codepointCountArtist = 0;
int *codepointsArtist = LoadCodepoints(current_artist.c_str(), &codepointCountArtist);

Vector2 artistPos = {150, 615};
float artistSize = 13.0f;
float artistScale = artistSize / customFont.baseSize;

for (int i = 0; i < codepointCountArtist; i++) 
{
    int cp = codepointsArtist[i];
    int index = GetGlyphIndex(customFont, cp);

    // main glyph (no shadow = cleaner hierarchy)
    DrawTextCodepoint(customFont, cp, artistPos, artistSize, (Color){140, 142, 153, 255});

    float advance = (customFont.glyphs[index].advanceX > 0) ?
        (float)customFont.glyphs[index].advanceX :
        (float)customFont.recs[index].width;

    artistPos.x += (advance * artistScale) + 1.0f;
}
UnloadCodepoints(codepointsArtist);
        
        // --- LUCID STYLE 3-BAND EQUALIZER ---
        DrawText("EQUALIZER", 60, 450, 11, (Color){100, 100, 110, 255});
        
        const char* eq_tags[] = {"BASS", "MID", "TREBLE"};
        
        for (int i = 0; i < 3; i++) {
            int eq_y = 475 + (i * 25);
            DrawText(eq_tags[i], 60, eq_y - 4, 10, (Color){140, 142, 153, 255});
            DrawRectangle(120, eq_y, 200, 2, (Color){35, 36, 42, 255});
            
            Rectangle slider_hitbox = {120, (float)eq_y - 10, 200, 20};
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, slider_hitbox)) {
                eq_vals[i] = (mouse.x - 120) / 200.0f;
                if (eq_vals[i] < 0.0f) eq_vals[i] = 0.0f;
                if (eq_vals[i] > 1.0f) eq_vals[i] = 1.0f;
            }
            
            int handle_x = 120 + (eq_vals[i] * 200);
            DrawRectangle(handle_x - 1, eq_y - 6, 2, 14, (Color){0, 255, 210, 255});
        } 
        
        // --- FORCE VERSION ON TOP OF EVERYTHING ---
        DrawText("v1.0.4-DRM", 1140, 670, 14, (Color){180, 180, 190, 255});

        EndDrawing();
    }

    system_running = false;
    meta_thread.join();
    audio_thread.join();

    CloseWindow();
    return 0;
}
