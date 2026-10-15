#include "dual_analog.hpp"
#include "psprecomp/common.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <string>

namespace psprecomp {
// The generated units that hold the original functions.
void recomp_unit_0008(Runtime &, AllegrexContext &);   // pad object, 0x08824000
void recomp_unit_0048(Runtime &, AllegrexContext &);   // Control and ControlSetup, 0x088C4000
}

namespace chili {
namespace {
// ControlSetup entry points the compiled game scripts call (setup in a0, name or id in a1).
constexpr std::uint32_t get_control_id_address = 0x088C7328u;
constexpr std::uint32_t is_control_down_address = 0x088C73B0u;
constexpr std::uint32_t is_control_pressed_address = 0x088C73E4u;
constexpr std::uint32_t is_control_released_address = 0x088C7424u;
constexpr std::uint32_t get_control_pressure_address = 0x088C7464u;
constexpr std::uint32_t get_control_real_pressure_address = 0x088C74A4u;
// Pad::GetAxisPosition(out vector in a0, pad in a1, axis in a2); the PSP build
// returns zero for every axis but 0.
constexpr std::uint32_t get_axis_position_address = 0x088247DCu;
// The engine reads no control while its pad is absent or input is switched off.
constexpr std::uint32_t pad_table_address = 0x08DF2650u;
constexpr std::uint32_t active_pad_index_address = 0x08DFB9BCu;
constexpr std::uint32_t input_disabled_address = 0x08DFB9CCu;
// The script entity being run; its data block is at +0x14.
constexpr std::uint32_t current_script_entity_address = 0x08DFB450u;
// Two places in the player script's movement function (0x08B7540C) where the
// two-stick path, which never shipped on the PSP, is not finished. Both are
// told apart by the return address of the query.
//
// It asks for look_down's pressure and drops the answer: the local the pitch
// is computed from (sp+0x1C) was set to zero just before and is only written
// on the one-stick path, so the view could pitch up and never down.
constexpr std::uint32_t look_down_dropped_return = 0x08B7563Cu, look_down_local = 0x1Cu;
// A look pressure that is not zero and pitches nothing: the pitch rate is one radian per second times its cube.
constexpr float held_pitch_pressure = 1.0e-4f;
// It builds m_vStickLeft, the left stick as a vector (the direction of a dive),
// from the real pressure of look_right and look_left, which on the PSP are the
// left stick's X axis and here are the right stick.
constexpr std::uint32_t stick_left_right_return = 0x08B757B0u, stick_left_left_return = 0x08B75818u;
// The same function's read of look_left, the first of the two turn controls it asks for each frame on foot.
constexpr std::uint32_t on_foot_look_left_return = 0x08B75658u;
// Scene::RealTimePassed, the frame time the player script multiplies the look rates by (0x08B75E60).
constexpr std::uint32_t real_frame_seconds_address = 0x08DFAE90u;
// The player's motion. master_control_input_ctrl keeps its move controller (character_animation_ctrl) at
// +0x30; that one's data has its state at +0x70 and its physics controller (character_physic_ctrl) at +0xF4.
// The physics controller steps at a fixed rate of its own (15 a second on foot): each step takes the turn
// rate the look controls asked for (its +0x24), turns by rate times step and leaves the step's two ends in its
// data. character_root (0x08A03ECC) then moves the player between the two ends frame by frame and hands the
// result to the physics controller's try_moving_here (0x089C3124), which sets the node. That is why a turn
// asked through the look controls is taken about every 70 ms and held.
//   +0xB4 / +0xE0    orientation at the start / the end of the step (quaternions)
//   +0x148           a node the player rides on; when set (and the move state is not 0x2B) the ends in use
//                    are the pair at +0x190 / +0x1A0, relative to that node
constexpr std::uint32_t mover_state = 0x70u, mover_physics = 0xF4u, mover_state_plain = 0x2Bu;
//   +0x2A0           physic_model: 0 on the ground (the steps above), 1 in the air, 3 held to a wall by a stunt
//   +0x2A4           physic_allow_turning: whether the steps take the turn rate at all
constexpr std::uint32_t physics_turn_rate = 0x24u, physics_step_seconds = 0x84u;
constexpr std::uint32_t physics_model = 0x2A0u, physics_allow_turning = 0x2A4u;
constexpr std::uint32_t physics_model_air = 1u, physics_model_wall = 3u;
constexpr std::uint32_t physics_start = 0xB4u, physics_end = 0xE0u, physics_carrier = 0x148u, physics_carried_start = 0x190u, physics_carried_end = 0x1A0u;
// Move states in which the script doubles its look rates (0x08B75E3C).
constexpr std::uint32_t mover_state_fast_look_a = 0x11u, mover_state_fast_look_b = 0x24u;
// Move states 27 to 30: flying through the air to the left, right, back or forward. That is a dive (Cross with
// the stick held: off the ground for the first frames, physic_model 0, then in the air in slow motion) and
// also a player thrown by a blow (in the air from its first frame). 52 is the player on the ground after
// either, getting up.
// In the air of those four states the physics steps do not turn the player. The move controller's own state
// does (character_Combat_Move_ctrl state_11, 0x0898C338): every frame it takes the node's orientation, turns
// it by the move controller's turn rate times 0.75 times RealTimePassed, and writes the node and both ends of
// the physics step with the result; what was written to the step ends before is lost. So there the angle
// goes in as the rate that makes it: 180 degrees per second times the pressure squared, as the script asks
// (0x08B75E60), times that 0.75.
constexpr std::uint32_t mover_state_dive_first = 27u, mover_state_dive_last = 30u, mover_state_floor = 52u;
constexpr float dive_turn_degrees_per_second = 180.0f * 0.75f;
// The second of the two turn reads of a frame on foot (look_right).
constexpr std::uint32_t on_foot_look_right_return = 0x08B75678u;
// On the ground after a dive the player's own script places him and the camera: character_player_ai state_12
// (0x089EDA74, the dive from take-off to getting up) eases the player from the orientation he landed with
// to the camera's, writes the node and the physics step with the result every frame (0x089EDF50), and hands
// the camera the orientation it had at the landing, every frame (0x089EE158). All three orientations are
// kept among the state's own variables; there the angle turns all three, so player and view turn together.
//   script thread +0x48   the state's frame: entity +0x00, resume address +0x08, index of its variables +0x0E (16 bit)
//   script thread +0x08   -> +0x04: the variable stack (words)
//   variables +0x00       the camera's orientation
//   variables +0x14       set while the state eases the player between the orientations at +0x2C and +0x1C
//   entity -> +0x14 data, whose +0x104 is the physics controller the state writes
constexpr std::uint32_t player_dive_state_address = 0x089EDA74u;
constexpr std::uint32_t thread_frame = 0x48u, thread_stack = 0x08u, frame_resume = 0x08u, frame_variables = 0x0Eu;
constexpr std::uint32_t dive_camera = 0x00u, dive_easing = 0x14u, dive_ease_to = 0x1Cu, dive_ease_from = 0x2Cu, player_ai_physics = 0x104u;

// The view's pitch. master_control_input_ctrl hands its look angles (+0x120, radians, pitch first, held to
// 45 degrees up and down at 0x08B760E4) to the player's script (+0x2C: character_player_ai), which gives
// them to its camera controller (camera_system, +0x108 of the script's data) as an orientation:
//   +0x24   end_orient: the look angles, as given
//   +0x34   current_orient: what the camera is drawn from; every frame it goes rotate_speed (+0xB4, 10 a
//           second) times the frame time of the way to end_orient (0x0892DED4): a third at 30 frames per
//           second. That is the late tilt: half of a pitch after two frames, 90% after four.
//   +0x190  m_nForcedCameraTilt, +0x194 _nActualCameraTilt: a tilt the level asks for in places while the
//           player looks up or down (camera_force_tilt_Ctrl writes the forced one every frame, 0x089250B0);
//           the actual one goes to it at 1 a second (0x0892DDE0) and is added to the view, which is the
//           part of a pitch the view gives back.
// The controller's update, once a frame for the script entity at 0x08DFB450, is the function at 0x0892DBC8.
constexpr std::uint32_t camera_update_address = 0x0892DBC8u;
constexpr std::uint32_t input_look = 0x120u, input_player_ai = 0x2Cu, player_ai_camera = 0x108u;
constexpr std::uint32_t camera_end_orient = 0x24u, camera_current_orient = 0x34u, camera_forced_tilt = 0x190u, camera_actual_tilt = 0x194u;
constexpr float look_pitch_limit = 0.7853982f;

float guest_float(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return memory.contains(address, 4u) ? std::bit_cast<float>(memory.load32(address)) : 0.0f;
}
std::uint32_t guest_word(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return memory.contains(address, 4u) ? memory.load32(address) : 0u;
}
// Turns the orientation at `address` (a quaternion, x y z w) by an angle about the vertical of the frame it
// is kept in: heading h is the quaternion (0, -sin(h/2), 0, cos(h/2)) here, and the result is the
// orientation times that. False, and nothing written, when the four numbers there are not a rotation.
bool turn_orientation(psprecomp::GuestMemory &memory, std::uint32_t address, float degrees) {
    if (!memory.contains(address, 16u)) return false;
    float q[4], norm = 0.0f;
    for (unsigned i = 0; i < 4u; ++i) { q[i] = std::bit_cast<float>(memory.load32(address + i * 4u)); norm += q[i] * q[i]; }
    if (!(norm > 0.98f && norm < 1.02f)) return false;
    const float half = degrees * (3.14159265f / 360.0f), ry = -std::sin(half), rw = std::cos(half);
    const float turned[4]{q[0] * rw - q[2] * ry, q[3] * ry + q[1] * rw, q[0] * ry + q[2] * rw, q[3] * rw - q[1] * ry};
    for (unsigned i = 0; i < 4u; ++i) memory.store32(address + i * 4u, std::bit_cast<std::uint32_t>(turned[i]));
    return true;
}
// The same about the side axis: a look pitched down by p is the quaternion (-sin(p/2), 0, 0, cos(p/2)).
bool pitch_orientation(psprecomp::GuestMemory &memory, std::uint32_t address, float radians) {
    if (!memory.contains(address, 16u)) return false;
    float q[4], norm = 0.0f;
    for (unsigned i = 0; i < 4u; ++i) { q[i] = std::bit_cast<float>(memory.load32(address + i * 4u)); norm += q[i] * q[i]; }
    if (!(norm > 0.98f && norm < 1.02f)) return false;
    const float rx = -std::sin(radians * 0.5f), rw = std::cos(radians * 0.5f);
    const float pitched[4]{q[3] * rx + q[0] * rw, q[1] * rw + q[2] * rx, q[2] * rw - q[1] * rx, q[3] * rw - q[0] * rx};
    for (unsigned i = 0; i < 4u; ++i) memory.store32(address + i * 4u, std::bit_cast<std::uint32_t>(pitched[i]));
    return true;
}
bool is_orientation(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    if (!memory.contains(address, 16u)) return false;
    float norm = 0.0f;
    for (unsigned i = 0; i < 4u; ++i) { const float v = std::bit_cast<float>(memory.load32(address + i * 4u)); norm += v * v; }
    return norm > 0.98f && norm < 1.02f;
}
// Scene nodes (Kapow's Node): parent +0x3C, first child +0x40, next sibling
// +0x44, name +0x48, and at +0x34 a pointer to the node's orientation
// quaternion (x y z w) followed by its position (x y z).
constexpr std::uint32_t node_transform = 0x34u, node_parent = 0x3Cu, node_child = 0x40u, node_sibling = 0x44u, node_name = 0x48u;
bool node_is_named(const psprecomp::GuestMemory &memory, std::uint32_t node, std::string_view name) {
    const auto text = guest_word(memory, node + node_name);
    if (!text || !memory.contains(text, name.size() + 1u)) return false;
    for (std::size_t i = 0; i <= name.size(); ++i)
        if (memory.load8(text + static_cast<std::uint32_t>(i)) != (i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0u)) return false;
    return true;
}
// Position, then heading and elevation in degrees of the node's forward axis:
// the third row of the matrix Kapow builds from the quaternion (the same
// NodeMatrix as Total Overdose). Heading 0 is +Z, positive towards +X, which
// is to the right; positive elevation is up.
void trace_transform(std::ostream &out, const psprecomp::GuestMemory &memory, std::uint32_t node) {
    const auto transform = node ? guest_word(memory, node + node_transform) : 0u;
    if (!transform || !memory.contains(transform, 28u)) { out << ",,,,,"; return; }
    const float x = guest_float(memory, transform), y = guest_float(memory, transform + 4u), z = guest_float(memory, transform + 8u),
        w = guest_float(memory, transform + 12u);
    const float forward_x = 2.0f * (x * z - w * y), forward_y = 2.0f * (y * z + w * x), forward_z = 1.0f - 2.0f * (x * x + y * y);
    constexpr float degrees = 57.29578f;
    for (const auto offset : {16u, 20u, 24u}) out << ',' << guest_float(memory, transform + offset);
    out << ',' << std::atan2(forward_x, forward_z) * degrees << ',' << std::asin(std::clamp(forward_y, -1.0f, 1.0f)) * degrees;
}
}

DualAnalog *DualAnalog::active_{};

DualAnalog::DualAnalog(psprecomp::Runtime &runtime, const std::filesystem::path &trace) : runtime_(runtime) {
    if (active_) throw psprecomp::Error("Only one dual-analog control layer may be active");
    for (const auto address : {get_control_id_address, is_control_down_address, is_control_pressed_address, is_control_released_address,
             get_control_pressure_address, get_control_real_pressure_address, get_axis_position_address})
        if (!runtime.has_function(address)) throw psprecomp::Error("Dual-analog controls: no game code at " + psprecomp::hex32(address));
    active_ = this;
    // Names must not start with "recomp_unit_": these are host functions, and
    // calls into their units have to come back through the dispatcher.
    runtime.register_function(get_control_id_address, &get_control_id, "chili_dual_analog_control_id");
    runtime.register_function(is_control_down_address, &is_control_down, "chili_dual_analog_down");
    runtime.register_function(is_control_pressed_address, &is_control_pressed, "chili_dual_analog_pressed");
    runtime.register_function(is_control_released_address, &is_control_released, "chili_dual_analog_released");
    runtime.register_function(get_control_pressure_address, &get_control_pressure, "chili_dual_analog_pressure");
    runtime.register_function(get_control_real_pressure_address, &get_control_real_pressure, "chili_dual_analog_real_pressure");
    runtime.register_function(get_axis_position_address, &get_axis_position, "chili_dual_analog_axis");
    if (!trace.empty()) {
        trace_.open(trace, std::ios::binary | std::ios::trunc);
        if (!trace_) throw psprecomp::Error("Cannot create dual-analog trace file: " + trace.string());
        trace_ << "sample,buttons,x,y,rx,ry,queries,state,move_x,move_y,move_z,turn,look_x,look_y,look_z,"
                  "player_x,player_y,player_z,player_heading,player_elevation,camera_x,camera_y,camera_z,camera_heading,camera_elevation,"
                  "stick_left_x,stick_left_y,mouse_turn,mouse_pitch,physics_turn,physics_step,physics_model,physics_turns\n";
    }
}
DualAnalog::~DualAnalog() { active_ = nullptr; }

void DualAnalog::sampled(ControllerInput input) {
    if (trace_.is_open() && samples_) trace_row();
    // The menu's scene is looked at while the script runs. When it stops asking, the answer stands: the
    // menu's scene is unloaded a few samples before the look controls count as idle.
    const bool asked = controls_.look_queried();
    // The mouse's angle belongs to its sample's frame. When the game took the previous sample and has not
    // asked for the look controls since, while it is still using them (two pad reads in one frame, a frame
    // that skipped the player script), that angle is still owed and is added to this one. Once the look
    // controls are idle (a menu, a cutscene) what was not asked for is gone.
    const bool owed = !asked && controls_.look_active();
    const auto &memory = runtime_.memory();
    const auto mover_data = mover(), data = physics();
    const auto state = mover_data ? guest_word(memory, mover_data + mover_state) : 0u, model = data ? guest_word(memory, data + physics_model) : 0u;
    const bool flying = state >= mover_state_dive_first && state <= mover_state_dive_last;
    float heading = 0.0f;
    const bool heading_known = player_heading(heading);
    // The frame in which a dive leaves the ground: the step's ends were turned, then the move controller took
    // the player over from the node and the angle went nowhere. It is owed to the next frame.
    const bool lost = asked && due_turn_ != 0.0f && due_path_ == TurnPath::Step && flying && model == physics_model_air && model_ != physics_model_air &&
        heading_known && heading_known_ && std::fabs(std::remainder(heading - heading_, 360.0f)) < 0.05f;
    const float carried = lost ? due_turn_ : 0.0f;
    // Did the frame that has just ended turn the player by the angle it was given? Counted for the log.
    if (asked && due_turn_ != 0.0f && heading_known && heading_known_ && !lost) {
        ++turn_frames_;
        const auto bin = std::min<std::size_t>(due_state_, turn_frames_missed_.size() - 1u);
        if (due_path_ == TurnPath::None) ++turn_frames_left_[bin];
        else if (due_path_ == TurnPath::Landing) ++turn_frames_landing_;
        else if (std::fabs(std::remainder(heading - heading_ - due_turn_, 360.0f)) < 0.05f) ++turn_frames_exact_;
        else ++turn_frames_missed_[bin];
    }
    heading_ = heading; heading_known_ = heading_known;
    controls_.sampled(input);
    if (asked) main_menu_ = main_menu_loaded();
    ++samples_; queries_ = 0u;
    exact_turn_ = (owed ? exact_turn_ : carried) + input.turn; exact_pitch_ = (owed ? exact_pitch_ : 0.0f) + input.pitch;
    due_turn_ = dive_rate_ = 0.0f;
    stick_taken_ = view_pitched_ = false;
    // Whose flight it is. A dive starts on the ground or off a wall; a blow throws the player into the air at once.
    if (flying && !flying_) own_dive_ = model != physics_model_air || model_ == physics_model_wall;
    else if (!flying && state != mover_state_floor) own_dive_ = false;
    flying_ = flying; model_ = model;
}

std::uint32_t DualAnalog::player_node() const {
    const auto &memory = runtime_.memory();
    auto node = script_data_ ? guest_word(memory, script_data_ + 0x30u) : 0u;
    for (unsigned depth = 0; node && depth < 8u && !node_is_named(memory, node, "PlayerCharacter"); ++depth)
        node = guest_word(memory, node + node_parent);
    return node && node_is_named(memory, node, "PlayerCharacter") ? node : 0u;
}
bool DualAnalog::player_heading(float &degrees) const {
    const auto &memory = runtime_.memory();
    const auto node = player_node(), transform = node ? guest_word(memory, node + node_transform) : 0u;
    if (!transform || !memory.contains(transform, 16u)) return false;
    const float x = guest_float(memory, transform), y = guest_float(memory, transform + 4u), z = guest_float(memory, transform + 8u),
        w = guest_float(memory, transform + 12u);
    degrees = std::atan2(2.0f * (x * z - w * y), 1.0f - 2.0f * (x * x + y * y)) * 57.29578f;
    return true;
}

std::string DualAnalog::turn_report() const {
    std::string text = "[input] frames with a turn to make=" + std::to_string(turn_frames_) + ": player turned by exactly that angle=" + std::to_string(turn_frames_exact_) +
        ", turned on the ground after a dive (where the game also eases him to the view)=" + std::to_string(turn_frames_landing_);
    const auto by_state = [&text](const char *what, const std::array<std::uint32_t, 64> &counts) {
        bool first = true;
        for (std::size_t state = 0; state < counts.size(); ++state) {
            if (!counts[state]) continue;
            text += (first ? what : ", ") + std::to_string(state) + " x" + std::to_string(counts[state]);
            first = false;
        }
    };
    by_state("; left to the game (stunt on a wall, knocked down), by move state: ", turn_frames_left_);
    by_state("; not exactly, by move state: ", turn_frames_missed_);
    return text;
}

// How this frame's angle reaches the player, from where the player is.
DualAnalog::TurnPath DualAnalog::turn_path() {
    const auto &memory = runtime_.memory();
    const auto mover_data = mover(), data = physics();
    if (!mover_data || !data) return TurnPath::Step;
    const auto state = guest_word(memory, mover_data + mover_state), model = guest_word(memory, data + physics_model);
    due_state_ = state;
    // Held to a wall by a stunt (running up it, the flip off it): the game places the player.
    if (model == physics_model_wall) return TurnPath::None;
    if (state >= mover_state_dive_first && state <= mover_state_dive_last && model == physics_model_air) return TurnPath::Dive;
    // On the ground after a flight: after a dive of his own the player turns as his script settles him;
    // knocked down by a blow he lies where the game put him until he is up.
    if (state == mover_state_floor) return own_dive_ ? TurnPath::Landing : TurnPath::None;
    return TurnPath::Step;
}

std::uint32_t DualAnalog::mover() const {
    const auto &memory = runtime_.memory();
    return script_data_ ? guest_word(memory, guest_word(memory, script_data_ + 0x30u) + 0x14u) : 0u;
}
std::uint32_t DualAnalog::physics() const {
    const auto &memory = runtime_.memory();
    const auto mover_data = mover();
    return mover_data ? guest_word(memory, guest_word(memory, mover_data + mover_physics) + 0x14u) : 0u;
}

// Turns both ends of the physics step in progress by the same angle about the vertical of the frame they are
// kept in (the world, or the node the player rides on). The
// frame's place between the two ends is then turned by exactly that angle, the next step starts from an end
// that has it, and the node gets it through the game's own path in this frame. The rate the look controls
// ask for is added by the game as before.
bool DualAnalog::turn_player(float degrees) {
    auto &memory = runtime_.memory();
    const auto mover_data = mover(), data = physics();
    if (!data || !memory.contains(data, physics_carried_end + 16u)) return false;
    const auto state = guest_word(memory, mover_data + mover_state);
    const bool carried = guest_word(memory, data + physics_carrier) != 0u && state != mover_state_plain;
    const std::uint32_t start = data + (carried ? physics_carried_start : physics_start), end = data + (carried ? physics_carried_end : physics_end);
    if (!is_orientation(memory, start) || !is_orientation(memory, end)) return false;
    return turn_orientation(memory, start, degrees) && turn_orientation(memory, end, degrees);
}

// The two orientations the player's dive state eases between on the ground and the camera's, turned by the
// same angle: every orientation between the two, which is what the state writes, is then turned by it too.
// False when the state is not running for this player or is not easing.
bool DualAnalog::turn_landing(float degrees) {
    auto &memory = runtime_.memory();
    if (!dive_thread_ || dive_thread_sample_ + 2u < samples_ || !memory.contains(dive_thread_, thread_frame + 4u)) return false;
    const auto frame = guest_word(memory, dive_thread_ + thread_frame), stack = guest_word(memory, guest_word(memory, dive_thread_ + thread_stack) + 4u);
    if (!frame || !stack || !memory.contains(frame, 16u) || !guest_word(memory, frame + frame_resume)) return false;
    const auto variables = stack + memory.load16(frame + frame_variables) * 4u;
    const auto ai = guest_word(memory, guest_word(memory, frame) + 0x14u);
    if (!ai || guest_word(memory, guest_word(memory, ai + player_ai_physics) + 0x14u) != physics()) return false;
    if (!memory.contains(variables, dive_ease_from + 16u) || !guest_word(memory, variables + dive_easing)) return false;
    for (const auto offset : {dive_camera, dive_ease_to, dive_ease_from}) if (!is_orientation(memory, variables + offset)) return false;
    for (const auto offset : {dive_camera, dive_ease_to, dive_ease_from}) turn_orientation(memory, variables + offset, degrees);
    return true;
}

void DualAnalog::set_exact_turn(bool on) {
    exact_stick_ = on;
    // One more host function over a guest address moves the guest clock (it counts dispatches), so a run
    // that keeps the game's own turning, as the reference replays do, does not get it.
    if (on) prepare_exact_turn();
}
void DualAnalog::prepare_exact_turn() {
    if (player_dive_state_ || !(player_dive_state_ = runtime_.function_at(player_dive_state_address))) return;
    runtime_.register_function(player_dive_state_address, &player_dive_state, "chili_dual_analog_player_dive");
}

// Entered once a frame while the player's script is in its dive state: remember its thread. With ExactTurn
// off (the function is there all the same in a run whose settings can change) nothing is remembered, and
// the ground after a dive stays the game's own, as without the function.
void DualAnalog::player_dive_state(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    if (active_->exact_stick_) {
        active_->dive_thread_ = ctx.gpr[4];
        active_->dive_thread_sample_ = active_->samples_;
    }
    active_->player_dive_state_(rt, ctx);
}

// The player script reads look_up, look_down, look_left, look_right once a frame on foot and makes rates
// of them: a turn of 171 degrees per second times the pressure squared, handed to the physics steps, and a
// pitch of one radian per second times the pressure cubed, applied in the same frame over RealTimePassed.
// The mouse's angle does not go through the turn rate: it turns the player at the first of the two turn
// reads. So does the second stick's (and the look keys') turn when the stick is exact: its rate times the
// frame is an angle, and both turn pressures are then answered as zero; otherwise the pressures are left to
// the stick. In the air of a dive, where the game turns the player every frame by itself, the angle is
// answered as the rate that makes exactly it in this frame; where the game places the player (a stunt on a
// wall, knocked down) it is dropped. The mouse's pitch is added to the stick's as the pressure that makes
// exactly that angle in this frame. A read from anywhere else gets the turn as a rate, as before.
float DualAnalog::exact_look(unsigned control, std::uint32_t caller, float pressure) {
    const bool turn = control == DualAnalogControls::LookLeft || control == DualAnalogControls::LookRight;
    // The other half of what the first of the two reads worked out.
    if (control == DualAnalogControls::LookRight && caller == on_foot_look_right_return) {
        if (dive_rate_ != 0.0f) {
            const float rate = dive_rate_;
            dive_rate_ = 0.0f;
            return rate > 0.0f ? std::sqrt(rate / dive_turn_degrees_per_second) : 0.0f;
        }
        if (stick_taken_) { stick_taken_ = false; return 0.0f; }
    }
    const auto input = controls_.input();
    const bool first_turn_read = control == DualAnalogControls::LookLeft && caller == on_foot_look_left_return;
    const bool stick = first_turn_read && exact_stick_ && input.rx != 128u;
    // The script also adds the turn pressures to a look angle of its own (Look.y: 5 radians a second times
    // the difference of their squares, held to 108 degrees, 0x08B75EF4) and eases it back only while the
    // player moves and the pitch is not held. With the port turning the player the pressures are zero and so
    // is that angle; the rate a dive is given would pile it up and leave it there. So it starts every frame
    // from zero: a frame's own share while the mouse turns in a dive, nothing afterwards.
    if (first_turn_read && exact_stick_ && script_data_ && runtime_.memory().contains(script_data_ + input_look, 12u))
        runtime_.memory().store32(script_data_ + input_look + 4u, 0u);
    if ((turn ? exact_turn_ == 0.0f && !stick : exact_pitch_ == 0.0f) || !input_enabled()) return pressure;
    const auto &memory = runtime_.memory();
    const float frame = guest_float(memory, real_frame_seconds_address);
    if (!(frame > 0.0f)) return pressure;
    if (turn) {
        if (first_turn_read) {
            const float stick_rate = DualAnalogControls::turn_of(input.rx);
            const float angle = exact_turn_ + (stick ? stick_rate * frame : 0.0f);
            // Without the exact stick a deflected one turns the player by the game's rate as well: not checked.
            const bool checked = stick || input.rx == 128u;
            due_path_ = turn_path();
            // On the ground after a dive the step's ends are turned as well, for the frames in which the
            // script has stopped writing them.
            if (due_path_ == TurnPath::Landing && turn_landing(angle)) turn_player(angle);
            if (due_path_ == TurnPath::Landing || (due_path_ == TurnPath::Step && turn_player(angle))) {
                due_turn_ = checked ? angle : 0.0f;
                exact_turn_ = 0.0f; stick_taken_ = stick;
                return stick ? 0.0f : pressure;
            }
            if (due_path_ == TurnPath::Dive) {
                dive_rate_ = stick_rate * (dive_turn_degrees_per_second / DualAnalogControls::turn_degrees_per_second) + exact_turn_ / frame;
                due_turn_ = dive_rate_ * frame;
                exact_turn_ = 0.0f;
                return dive_rate_ < 0.0f ? std::sqrt(-dive_rate_ / dive_turn_degrees_per_second) : 0.0f;
            }
            if (due_path_ == TurnPath::None) {
                due_turn_ = exact_turn_;
                exact_turn_ = 0.0f;
                return pressure;
            }
            // The physics state is not where it is expected: as a rate, below.
        }
        const float rate = DualAnalogControls::turn_of(input.rx) + exact_turn_ / frame;
        return (control == DualAnalogControls::LookRight) == (rate > 0.0f) ? std::sqrt(std::fabs(rate) / DualAnalogControls::turn_degrees_per_second) : 0.0f;
    }
    const auto state = guest_word(memory, mover() + mover_state);
    const float scale = state == mover_state_fast_look_a || state == mover_state_fast_look_b ? 0.5f : 1.0f;
    if (tight_view_ && !view_pitched_) { view_pitched_ = true; pitch_view(exact_pitch_); }
    const float rate = DualAnalogControls::pitch_of(input.ry) + exact_pitch_ * scale / frame;
    return (control == DualAnalogControls::LookDown) == (rate > 0.0f) ? std::cbrt(std::fabs(rate) / DualAnalogControls::pitch_degrees_per_second) : 0.0f;
}

// The mouse's pitch for the view as well: the camera's current orientation is pitched by the angle the look
// is about to be pitched by, so the camera does not fall behind by it and then catch up. What the camera
// still has to catch up with (the stick's pitch, the game levelling the view) it eases as before.
void DualAnalog::pitch_view(float degrees) {
    auto &memory = runtime_.memory();
    const auto ai = script_data_ ? guest_word(memory, guest_word(memory, script_data_ + input_player_ai) + 0x14u) : 0u;
    const auto camera = ai ? guest_word(memory, guest_word(memory, ai + player_ai_camera) + 0x14u) : 0u;
    if (!camera || !memory.contains(camera, camera_actual_tilt + 4u) || !is_orientation(memory, camera + camera_end_orient)) return;
    const float look = guest_float(memory, script_data_ + input_look);
    const float change = std::clamp(look + degrees * (3.14159265f / 180.0f), -look_pitch_limit, look_pitch_limit) - std::clamp(look, -look_pitch_limit, look_pitch_limit);
    if (change != 0.0f) pitch_orientation(memory, camera + camera_current_orient, change);
}

void DualAnalog::set_tight_view(bool on) {
    tight_view_ = on;
    if (!on || camera_update_ || !(camera_update_ = runtime_.function_at(camera_update_address))) return;
    runtime_.register_function(camera_update_address, &camera_update, "chili_dual_analog_camera_update");
}

// The tilt a level adds to the view while the player looks up or down, taken out as the player's camera
// controller starts its update: with the mouse the player is looking all the time, and the view would rise
// or sink by itself wherever a level asks for it.
void DualAnalog::camera_update(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &self = *active_;
    auto &memory = rt.memory();
    const auto camera = guest_word(memory, guest_word(memory, current_script_entity_address) + 0x14u);
    const auto ai = self.script_data_ ? guest_word(memory, guest_word(memory, self.script_data_ + input_player_ai) + 0x14u) : 0u;
    if (self.tight_view_ && camera && ai && camera == guest_word(memory, guest_word(memory, ai + player_ai_camera) + 0x14u) &&
        memory.contains(camera, camera_actual_tilt + 4u)) {
        memory.store32(camera + camera_forced_tilt, 0u);
        memory.store32(camera + camera_actual_tilt, 0u);
    }
    self.camera_update_(rt, ctx);
}

// The main menu's scene, looked up in the scene tree from the player script's node (menu_scene.hpp). A few
// guest loads once per controller sample.
bool DualAnalog::main_menu_loaded() {
    const auto &memory = runtime_.memory();
    const auto node = script_data_ ? guest_word(memory, script_data_ + 0x30u) : 0u;
    return node && menu_.loaded(memory, node);
}

// What the game did with the previous sample, read from the player script's
// data: master_control_input_ctrl keeps the player's move controller at +0x30
// (state +0x70, move vector +0x48, turn rate +0x54), its look angles at +0x120
// and m_vStickLeft at +0x174.
// The move controller is a node below PlayerCharacter, whose sibling GameCamera
// is the camera the scene is drawn from.
void DualAnalog::trace_row() {
    const auto &memory = runtime_.memory();
    const auto input = controls_.input();
    trace_ << (samples_ - 1u) << ',' << input.buttons << ',' << unsigned(input.x) << ',' << unsigned(input.y) << ','
           << unsigned(input.rx) << ',' << unsigned(input.ry) << ',' << queries_;
    const auto mover = this->mover();
    if (mover) {
        trace_ << ',' << guest_word(memory, mover + 0x70u);
        for (const auto offset : {0x48u, 0x4Cu, 0x50u, 0x54u}) trace_ << ',' << guest_float(memory, mover + offset);
        for (const auto offset : {0x120u, 0x124u, 0x128u}) trace_ << ',' << guest_float(memory, script_data_ + offset);
    } else trace_ << ",,,,,,,,";
    const std::uint32_t player = player_node();
    std::uint32_t camera = player ? guest_word(memory, guest_word(memory, player + node_parent) + node_child) : 0u;
    for (unsigned count = 0; camera && count < 64u && !node_is_named(memory, camera, "GameCamera"); ++count)
        camera = guest_word(memory, camera + node_sibling);
    trace_transform(trace_, memory, player);
    trace_transform(trace_, memory, camera);
    if (script_data_) trace_ << ',' << guest_float(memory, script_data_ + 0x174u) << ',' << guest_float(memory, script_data_ + 0x178u);
    else trace_ << ",,";
    // The mouse's angle of the sample, and the physics controller's turn rate and step length.
    trace_ << ',' << input.turn << ',' << input.pitch;
    if (const auto data = physics()) trace_ << ',' << guest_float(memory, data + physics_turn_rate) << ',' << guest_float(memory, data + physics_step_seconds)
                                            << ',' << guest_word(memory, data + physics_model) << ',' << guest_word(memory, data + physics_allow_turning);
    else trace_ << ",,,,";
    trace_ << '\n';
    trace_.flush();
}

bool DualAnalog::input_enabled() const {
    const auto &memory = runtime_.memory();
    const auto pad = guest_word(memory, active_pad_index_address);
    return pad == 0u && guest_word(memory, pad_table_address) != 0u && memory.load8(input_disabled_address + pad) == 0u;
}

bool DualAnalog::answer(psprecomp::AllegrexContext &ctx, DualAnalogControls::Answer &out) {
    const auto id = ctx.gpr[5];
    if (!DualAnalogControls::is_id(id)) return false;
    const auto &memory = runtime_.memory();
    const auto control = id - DualAnalogControls::first_id;
    out = controls_.query(control, input_enabled());
    ++queries_;
    if (control == DualAnalogControls::LookLeft) {
        // Remember the player script's data block, for the trace and for finding the scene tree: it is the
        // one whose look_left control id field holds the id just asked for.
        const auto data = guest_word(memory, guest_word(memory, current_script_entity_address) + 0x14u);
        if (data && guest_word(memory, data + 0x7Cu) == id) script_data_ = data;
    }
    ctx.pc = ctx.gpr[31];
    return true;
}

void DualAnalog::get_control_id(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    char name[16]{};
    std::size_t length = 0;
    const auto &memory = rt.memory();
    for (const auto address = ctx.gpr[5]; length < sizeof(name) && memory.contains(address + static_cast<std::uint32_t>(length)); ++length)
        if (!(name[length] = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(length))))) break;
    const auto control = length < sizeof(name) ? DualAnalogControls::from_name({name, length}) : DualAnalogControls::Count;
    if (control == DualAnalogControls::Count) { psprecomp::recomp_unit_0048(rt, ctx); return; }
    ctx.set_gpr(2u, DualAnalogControls::first_id + control);
    ctx.pc = ctx.gpr[31];
}
void DualAnalog::is_control_down(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    DualAnalogControls::Answer result;
    if (active_->answer(ctx, result)) ctx.set_gpr(2u, result.pressure > 0.0f ? 1u : 0u);
    else psprecomp::recomp_unit_0048(rt, ctx);
}
void DualAnalog::is_control_pressed(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    DualAnalogControls::Answer result;
    if (active_->answer(ctx, result)) ctx.set_gpr(2u, result.pressed ? 1u : 0u);
    else psprecomp::recomp_unit_0048(rt, ctx);
}
void DualAnalog::is_control_released(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    DualAnalogControls::Answer result;
    if (active_->answer(ctx, result)) ctx.set_gpr(2u, result.released ? 1u : 0u);
    else psprecomp::recomp_unit_0048(rt, ctx);
}
void DualAnalog::get_control_pressure(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    DualAnalogControls::Answer result;
    const auto caller = ctx.gpr[31];
    const auto control = ctx.gpr[5] - DualAnalogControls::first_id;
    const bool look_down_dropped = caller == look_down_dropped_return && control == DualAnalogControls::LookDown;
    if (!active_->answer(ctx, result)) { psprecomp::recomp_unit_0048(rt, ctx); return; }
    if (control >= DualAnalogControls::LookUp) result.pressure = active_->exact_look(control, caller, result.pressure);
    ctx.fpr[0] = result.pressure;
    // Put look_down where the caller's one-stick path stores it. That local and look_up's are also what the
    // script compares with zero (0x08B7641C) before it levels the view; see set_hold_pitch.
    if (look_down_dropped && rt.memory().contains(ctx.gpr[29] + look_down_local, 4u))
        rt.memory().store32(ctx.gpr[29] + look_down_local,
            std::bit_cast<std::uint32_t>(result.pressure == 0.0f && active_->hold_pitch_ ? held_pitch_pressure : result.pressure));
}
void DualAnalog::get_control_real_pressure(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    DualAnalogControls::Answer result;
    const auto caller = ctx.gpr[31];
    if (!active_->answer(ctx, result)) { psprecomp::recomp_unit_0048(rt, ctx); return; }
    // m_vStickLeft is the left stick.
    if (caller == stick_left_right_return || caller == stick_left_left_return)
        result.real_pressure = active_->input_enabled()
            ? active_->controls_.force(caller == stick_left_right_return ? DualAnalogControls::RunRight : DualAnalogControls::RunLeft) : 0.0f;
    ctx.fpr[0] = result.real_pressure;
}
void DualAnalog::get_axis_position(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const auto out = ctx.gpr[4];
    if (ctx.gpr[6] != 1u || !rt.memory().contains(out, 16u)) { psprecomp::recomp_unit_0008(rt, ctx); return; }
    // Axis 1: (x, y, 0, 0) in the output vector and in VFPU column C000, as the
    // original leaves them for axis 0.
    const std::array<float, 4> vector{active_->controls_.axis_x(), active_->controls_.axis_y(), 0.0f, 0.0f};
    for (std::uint32_t i = 0; i < 4u; ++i) {
        const auto bits = std::bit_cast<std::uint32_t>(vector[i]);
        rt.memory().store32(out + i * 4u, bits);
        ctx.set_vfpu_scalar_bits(i << 5u, bits);
    }
    ctx.set_gpr(2u, out);
    ctx.pc = ctx.gpr[31];
}
}
