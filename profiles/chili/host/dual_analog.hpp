#pragma once
#include "dual_analog_controls.hpp"
#include "menu_scene.hpp"
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
namespace chili {
// Installs the scheme into a running game: host functions at the guest
// addresses of the control-setup queries and of the pad's axis read. Create it
// after psprecomp::register_generated_functions, and only when the scheme is
// wanted; without it the game is untouched.
class DualAnalog {
public:
    // `trace`: optional CSV, one row per controller sample, of the input and of
    // what the player script made of it (diagnostics for scripted runs).
    DualAnalog(psprecomp::Runtime &runtime, const std::filesystem::path &trace = {});
    ~DualAnalog();
    DualAnalog(const DualAnalog &) = delete;
    DualAnalog &operator=(const DualAnalog &) = delete;
    void sampled(ControllerInput input);
    // Whether the player is being steered: the game asks for the look controls and the main menu is not
    // up. The player script also runs behind the main menu, where its character is part of the backdrop.
    bool look_active() const noexcept { return controls_.look_active() && !main_menu_; }
    // Mouse look: the view keeps the pitch it was given. The player script levels the view while the player
    // moves and both look-up and look-down read exactly zero; with this on, a look-down at rest is answered
    // to that one check as a pressure far too small to pitch (its cube is the rate), so the levelling never
    // starts. Off, the engine behaves as it does for a thumb stick.
    void set_hold_pitch(bool on) noexcept { hold_pitch_ = on; }
    // [Input] ExactTurn. The second stick and the look keys turn the player by each frame's angle, like the
    // mouse, instead of through the game's turn rate, which its physics steps take about every 70 ms and
    // hold (the pitch already is a rate the game applies every frame); and mouse and stick go on turning
    // the player on the ground after a dive, where the game itself does not turn him. Call it before the
    // game runs: turning on the ground needs a host function over the player script's dive state.
    void set_exact_turn(bool on);
    // For a run in which ExactTurn can change while the game runs (the game's own settings page): the host
    // function over the player's dive state is put in place now, before the game runs, whatever the setting
    // says; it does nothing while ExactTurn is off. switch_exact_turn then only sets the switch. Not for a
    // deterministic run: one more host function over a guest address moves its guest clock.
    void prepare_exact_turn();
    // ExactTurn while the game runs. False, and nothing changed, when switching it on needs the host
    // function and prepare_exact_turn was not called: a host function is not put over guest code that runs.
    bool switch_exact_turn(bool on) noexcept {
        if (on && !player_dive_state_) return false;
        exact_stick_ = on;
        return true;
    }
    bool exact_turn() const noexcept { return exact_stick_; }
    // [Game] Camera=tight. The view pitches with the mouse in the frame it moved, instead of easing a third
    // of the way a frame, and the tilt a level adds while the player looks up or down is taken out. Call it
    // before the game runs: the second part is a host function over the camera controller's update.
    void set_tight_view(bool on);
    // For the log: in how many frames there was an angle to turn the player by (the mouse's, the exact
    // stick's), in how many of them the player's heading then changed by exactly that angle, and the move
    // states of the others.
    std::uint64_t turn_frames() const noexcept { return turn_frames_; }
    std::string turn_report() const;

private:
    static DualAnalog *active_;
    static void get_control_id(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void is_control_down(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void is_control_pressed(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void is_control_released(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void get_control_pressure(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void get_control_real_pressure(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    static void get_axis_position(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    // Answers a query for a host-defined control; false when the id is the game's own.
    bool answer(psprecomp::AllegrexContext &, DualAnalogControls::Answer &);
    // The engine reads no control while its pad is absent or input is switched off.
    bool input_enabled() const;
    // Whether the main menu's scene is loaded (read from the game's scene tree).
    bool main_menu_loaded();
    void trace_row();
    // The player's move controller and physics controller data, from the player script's; 0 when not there.
    std::uint32_t mover() const;
    std::uint32_t physics() const;
    // The node the player is drawn at, and the heading of its forward axis; 0 and false when not there.
    std::uint32_t player_node() const;
    bool player_heading(float &degrees) const;
    // Turns the player by an angle now. False when the player's physics state is not where it is expected.
    bool turn_player(float degrees);
    // The same on the ground after a dive, where the player's own script places him.
    bool turn_landing(float degrees);
    static void player_dive_state(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    // Step: the two ends of the physics step are turned. Dive: the game's own turning in the air of a dive
    // is given the rate that makes the angle. Landing: the orientations the player's script settles him
    // between are turned. None: the game places the player and the angle is dropped.
    enum class TurnPath : std::uint8_t { Step, Dive, Landing, None };
    TurnPath turn_path();
    // A look control's pressure with the sample's exact mouse angle taken into account.
    float exact_look(unsigned control, std::uint32_t caller, float pressure);
    void pitch_view(float degrees);
    static void camera_update(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    psprecomp::Runtime &runtime_;
    DualAnalogControls controls_;
    std::ofstream trace_;
    std::uint64_t samples_{};
    std::uint32_t script_data_{};
    MenuScene menu_;
    unsigned queries_{};
    bool main_menu_{};
    bool hold_pitch_{}, exact_stick_{}, tight_view_{};
    // The camera has been pitched by this frame's mouse angle.
    bool view_pitched_{};
    float exact_turn_{}, exact_pitch_{};
    // Degrees per second the game's own dive turning is asked for in this frame (the look_right half), and
    // whether the first turn read took the stick's turn as an angle (the look_right half is then zero).
    float dive_rate_{};
    bool stick_taken_{};
    // The player is in the air of a dive or a blow, whether it is a dive of his own, and the physics model
    // of the previous sample.
    bool flying_{}, own_dive_{};
    std::uint32_t model_{};
    // The original of the player's dive state, the script thread it last ran in, and when.
    psprecomp::Runtime::RecompiledFunction player_dive_state_{}, camera_update_{};
    std::uint32_t dive_thread_{};
    std::uint64_t dive_thread_sample_{};
    // The angle the frame in progress is to turn the player by, the heading it started from, and the move
    // state and the way the angle was applied in.
    float due_turn_{}, heading_{};
    bool heading_known_{};
    std::uint32_t due_state_{};
    TurnPath due_path_{TurnPath::Step};
    std::uint64_t turn_frames_{}, turn_frames_exact_{}, turn_frames_landing_{};
    std::array<std::uint32_t, 64> turn_frames_missed_{}, turn_frames_left_{};
};
}
