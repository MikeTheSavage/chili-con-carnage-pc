// The two-stick control layer's turn and pitch over a made-up player in guest memory: where a frame's angle
// goes on foot, in the air of a dive, on the ground after a dive and after a blow, on a wall; the exact
// stick; the view's pitch and the level tilt; the count for the log. The guest structures are the ones
// host/dual_analog.cpp names; the game code under the hooks is replaced by functions that only return.
#include "dual_analog.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace psprecomp {
// The generated units dual_analog.cpp falls back to for controls that are not its own: not reached here.
void recomp_unit_0008(Runtime &, AllegrexContext &ctx) { ctx.pc = ctx.gpr[31]; }
void recomp_unit_0048(Runtime &, AllegrexContext &ctx) { ctx.pc = ctx.gpr[31]; }
}

namespace {
unsigned checks;
void require(bool ok, const char *what) { ++checks; if (!ok) throw std::runtime_error(what); }
bool close_to(double a, double b, double tolerance = 1.0e-3) { return std::fabs(a - b) <= tolerance; }
using Controls = chili::DualAnalogControls;

// Guest addresses of the game (host/dual_analog.cpp).
constexpr std::uint32_t pressure_query = 0x088C7464u, on_foot_left = 0x08B75658u, on_foot_right = 0x08B75678u, look_down_site = 0x08B7563Cu;
constexpr std::uint32_t dive_state = 0x089EDA74u, camera_update = 0x0892DBC8u;
constexpr std::uint32_t current_entity = 0x08DFB450u, pad_table = 0x08DF2650u, frame_seconds = 0x08DFAE90u;
// The made-up player.
constexpr std::uint32_t script = 0x09000000u, script_data = 0x09000100u, mover = 0x09000400u, mover_data = 0x09000500u,
    physics = 0x09000700u, physics_data = 0x09000800u, ai = 0x09000C00u, ai_data = 0x09000D00u, camera = 0x09000F00u, camera_data = 0x09001000u,
    player_node = 0x09001400u, player_name = 0x09001500u, player_transform = 0x09001600u, thread = 0x09002000u, frame = 0x09002100u,
    stack = 0x09002200u, variables = 0x09002300u, guest_stack = 0x09003000u, other_camera = 0x09003400u, other_camera_data = 0x09003500u;
constexpr float dt = 1.0f / 30.0f;

struct Player {
    psprecomp::Runtime rt;
    chili::DualAnalog dual;
    static void returns(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.pc = ctx.gpr[31]; }
    static psprecomp::Runtime &with_game_code(psprecomp::Runtime &runtime) {
        for (const auto address : {0x088C7328u, 0x088C73B0u, 0x088C73E4u, 0x088C7424u, pressure_query, 0x088C74A4u, 0x088247DCu, dive_state, camera_update})
            runtime.register_function(address, &returns, "test_game_code");
        return runtime;
    }
    Player() : dual(with_game_code(rt)) {
        auto &m = rt.memory();
        const auto link = [&m](std::uint32_t at, std::uint32_t value) { m.store32(at, value); };
        link(current_entity, script); link(script + 0x14u, script_data);
        link(script_data + 0x7Cu, Controls::first_id + Controls::LookLeft);
        link(script_data + 0x30u, mover); link(mover + 0x14u, mover_data); link(mover_data + 0xF4u, physics); link(physics + 0x14u, physics_data);
        link(script_data + 0x2Cu, ai); link(ai + 0x14u, ai_data); link(ai_data + 0x104u, physics); link(ai_data + 0x108u, camera); link(camera + 0x14u, camera_data);
        link(other_camera + 0x14u, other_camera_data);
        // The node the player is drawn at: the mover's parent, by name.
        link(mover + 0x3Cu, player_node); link(player_node + 0x48u, player_name); link(player_node + 0x34u, player_transform);
        const char name[] = "PlayerCharacter";
        for (std::uint32_t i = 0; i < sizeof(name); ++i) m.store8(player_name + i, static_cast<std::uint8_t>(name[i]));
        // The player script's dive state: its thread, frame and variables.
        link(thread + 0x48u, frame); link(frame, ai); link(frame + 0x08u, dive_state + 0x100u); m.store16(frame + 0x0Eu, 4u);
        link(thread + 0x08u, stack); link(stack + 4u, variables - 16u);
        link(pad_table, 1u);
        number(frame_seconds, dt);
        for (const auto at : {physics_data + 0xB4u, physics_data + 0xE0u, player_transform, camera_data + 0x24u, camera_data + 0x34u,
                 variables + 0x00u, variables + 0x1Cu, variables + 0x2Cu}) level(at);
        stand();
        // One frame of the player script asking for the turn controls: the layer finds the player by it.
        dual.sampled({});
        (void)turn_reads();
    }
    void number(std::uint32_t at, float value) { rt.memory().store32(at, std::bit_cast<std::uint32_t>(value)); }
    float number(std::uint32_t at) { return std::bit_cast<float>(rt.memory().load32(at)); }
    void level(std::uint32_t at) { number(at, 0.0f); number(at + 4u, 0.0f); number(at + 8u, 0.0f); number(at + 12u, 1.0f); }
    // Heading and pitch (down positive) of an orientation, degrees.
    float heading(std::uint32_t at) {
        const float x = number(at), y = number(at + 4u), z = number(at + 8u), w = number(at + 12u);
        return std::atan2(2.0f * (x * z - w * y), 1.0f - 2.0f * (x * x + y * y)) * 57.29578f;
    }
    float pitch(std::uint32_t at) { return -2.0f * std::asin(number(at)) * 57.29578f; }
    void state(std::uint32_t move, std::uint32_t model) { rt.memory().store32(mover_data + 0x70u, move); rt.memory().store32(physics_data + 0x2A0u, model); }
    void stand() { state(0u, 0u); }
    // What the game would do with the node after a frame that turned the player.
    void node_follows(float degrees) {
        const float half = degrees * (3.14159265f / 360.0f);
        const float q[4]{number(player_transform), number(player_transform + 4u), number(player_transform + 8u), number(player_transform + 12u)};
        const float ry = -std::sin(half), rw = std::cos(half);
        number(player_transform, q[0] * rw - q[2] * ry); number(player_transform + 4u, q[3] * ry + q[1] * rw);
        number(player_transform + 8u, q[0] * ry + q[2] * rw); number(player_transform + 12u, q[3] * rw - q[1] * ry);
    }
    chili::ControllerInput mouse(float turn, float pitch_angle = 0.0f) { chili::ControllerInput input; input.turn = turn; input.pitch = pitch_angle; return input; }
    chili::ControllerInput stick(std::uint8_t rx) { chili::ControllerInput input; input.rx = rx; return input; }
    float pressure(unsigned control, std::uint32_t caller) {
        auto ctx = rt.cpu();
        ctx.gpr[5] = Controls::first_id + control; ctx.gpr[31] = caller; ctx.gpr[29] = guest_stack;
        rt.function_at(pressure_query)(rt, ctx);
        return ctx.fpr[0];
    }
    // The two turn reads of a frame on foot.
    std::pair<float, float> turn_reads() { const float l = pressure(Controls::LookLeft, on_foot_left); return {l, pressure(Controls::LookRight, on_foot_right)}; }
    void run_dive_state() { auto ctx = rt.cpu(); ctx.gpr[4] = thread; ctx.gpr[31] = 0u; rt.function_at(dive_state)(rt, ctx); }
};
}

int main() {
    try {
        // On foot: both ends of the physics step are turned by the angle, the pressures stay the stick's.
        {
            Player p;
            p.dual.sampled(p.mouse(30.0f));
            const auto [l, r] = p.turn_reads();
            require(l == 0.0f && r == 0.0f, "on foot the mouse leaves the turn pressures alone");
            require(close_to(p.heading(physics_data + 0xB4u), 30.0) && close_to(p.heading(physics_data + 0xE0u), 30.0), "both ends of the step turned by the angle");
            // A second pair of reads in the same frame turns nothing more.
            (void)p.turn_reads();
            require(close_to(p.heading(physics_data + 0xB4u), 30.0), "the angle is applied once");
            p.node_follows(30.0f);
            p.dual.sampled(p.mouse(-12.5f));
            (void)p.turn_reads();
            require(close_to(p.heading(physics_data + 0xE0u), 17.5), "the next frame's angle");
            p.node_follows(-10.0f);     // the game turned him by something else
            p.dual.sampled({});
            const auto report = p.dual.turn_report();
            require(p.dual.turn_frames() == 2u && report.find("exactly that angle=1,") != std::string::npos && report.find("not exactly, by move state: 0 x1") != std::string::npos,
                "the log counts the frames whose heading changed by the angle, and the move state of the others");
        }
        // In the air of a dive the game turns the player itself: the angle goes in as the rate that makes it,
        // 180 degrees per second times the pressure squared times 0.75, and the step is left alone.
        {
            Player p;
            p.state(30u, 0u);
            p.dual.sampled({});
            p.state(30u, 1u);
            p.dual.sampled(p.mouse(3.0f));
            auto [l, r] = p.turn_reads();
            require(l == 0.0f && close_to(r, std::sqrt(3.0 / dt / 135.0), 1.0e-4), "a turn to the right in a dive is look_right's pressure");
            require(close_to(180.0 * r * r * 0.75 * dt, 3.0, 1.0e-4), "which the game's dive turning makes the angle from");
            require(p.heading(physics_data + 0xB4u) == 0.0f, "the step is not touched in the air of a dive");
            p.dual.sampled(p.mouse(-6.0f));
            std::tie(l, r) = p.turn_reads();
            require(r == 0.0f && close_to(l, std::sqrt(6.0 / dt / 135.0), 1.0e-4), "to the left it is look_left's");
            // The stick in a dive keeps the game's own path and adds to the mouse.
            p.dual.sampled(p.stick(196u));
            std::tie(l, r) = p.turn_reads();
            require(l == 0.0f && close_to(r, Controls::pressure_of(Controls::raw(196u)) * Controls::look_gain, 1.0e-4), "the stick alone in a dive: the game's own pressure");
        }
        // The frame in which a dive leaves the ground loses the angle given to the step: it is owed to the next.
        {
            Player p;
            p.state(30u, 0u);
            p.dual.sampled(p.mouse(3.0f));
            (void)p.turn_reads();
            require(close_to(p.heading(physics_data + 0xB4u), 3.0), "taking off, the step is turned");
            p.state(30u, 1u);                       // the move controller took over; the node did not turn
            p.dual.sampled(p.mouse(3.0f));
            const auto [l, r] = p.turn_reads();
            require(l == 0.0f && close_to(r, std::sqrt(6.0 / dt / 135.0), 1.0e-4), "the lost angle is added to the next frame's");
        }
        // On the ground after a dive of his own the player's script places him: its two orientations and the
        // camera's are turned. After a blow nothing is. Held to a wall nothing is.
        {
            Player p;
            p.dual.set_exact_turn(true);
            p.rt.memory().store32(variables + 0x14u, 1u);
            p.state(30u, 0u); p.dual.sampled({});   // a dive: starts on the ground
            p.state(30u, 1u); p.dual.sampled({});
            p.state(52u, 0u);
            p.run_dive_state();
            p.dual.sampled(p.mouse(5.0f));
            const auto [l, r] = p.turn_reads();
            require(l == 0.0f && r == 0.0f, "on the ground the pressures stay the stick's");
            for (const auto at : {variables + 0x00u, variables + 0x1Cu, variables + 0x2Cu, physics_data + 0xB4u, physics_data + 0xE0u})
                require(close_to(p.heading(at), 5.0), "after a dive the script's orientations, the camera's and the step are turned");
            // The script is no longer easing: nothing to turn.
            p.rt.memory().store32(variables + 0x14u, 0u);
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            require(close_to(p.heading(variables + 0x1Cu), 5.0), "not easing, not turned");
        }
        // A run whose settings can change while the game runs (the game's own settings page): the host
        // function over the dive state is in place from the start and does nothing while ExactTurn is off;
        // the switch then works without a new host function.
        {
            Player p;
            require(!p.dual.switch_exact_turn(true) && !p.dual.exact_turn(), "without the host function ExactTurn cannot be switched on while the game runs");
            require(p.dual.switch_exact_turn(false), "switching it off needs nothing");
            p.dual.prepare_exact_turn();
            require(!p.dual.exact_turn(), "putting the host function in place does not switch ExactTurn on");
            p.rt.memory().store32(variables + 0x14u, 1u);
            p.state(30u, 0u); p.dual.sampled({});
            p.state(30u, 1u); p.dual.sampled({});
            p.state(52u, 0u);
            p.run_dive_state();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            require(p.heading(variables + 0x1Cu) == 0.0f && p.heading(physics_data + 0xB4u) == 0.0f, "ExactTurn off: on the ground after a dive nothing turns the player, as without the host function");
            require(p.dual.switch_exact_turn(true) && p.dual.exact_turn(), "switched on while the game runs");
            p.run_dive_state();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            require(close_to(p.heading(variables + 0x1Cu), 5.0) && close_to(p.heading(physics_data + 0xB4u), 5.0), "and from then on the mouse turns him on the ground after a dive");
            require(p.dual.switch_exact_turn(false) && !p.dual.exact_turn(), "and off again");
            p.run_dive_state();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            const float stopped = p.heading(variables + 0x1Cu);
            require(close_to(stopped, 5.0) || close_to(stopped, 10.0), "switched off, the turning on the ground stops within two samples");
            p.run_dive_state();
            p.dual.sampled(p.mouse(5.0f));
            (void)p.turn_reads();
            require(p.heading(variables + 0x1Cu) == stopped, "and stays stopped while the dive state runs on");
        }
        {
            Player thrown;
            thrown.dual.set_exact_turn(true);
            thrown.rt.memory().store32(variables + 0x14u, 1u);
            thrown.dual.sampled({});
            thrown.state(28u, 1u); thrown.dual.sampled({});   // in the air from the first frame: a blow
            thrown.state(52u, 0u);
            thrown.run_dive_state();
            thrown.dual.sampled(thrown.mouse(5.0f));
            (void)thrown.turn_reads();
            require(thrown.heading(variables + 0x1Cu) == 0.0f && thrown.heading(physics_data + 0xB4u) == 0.0f, "knocked down, the player is left to the game");
            thrown.stand();
            thrown.dual.sampled(thrown.mouse(5.0f));
            (void)thrown.turn_reads();
            require(close_to(thrown.heading(physics_data + 0xB4u), 5.0), "up again, he turns");
        }
        {
            Player wall;
            wall.state(57u, 3u);
            wall.dual.sampled(wall.mouse(5.0f));
            (void)wall.turn_reads();
            require(wall.heading(physics_data + 0xB4u) == 0.0f, "held to a wall, the player is left to the game");
            wall.dual.sampled({});
            require(wall.dual.turn_report().find("left to the game (stunt on a wall, knocked down), by move state: 57 x1") != std::string::npos, "and the log says so");
            // The dive off the wall is his own.
            wall.state(30u, 1u); wall.dual.sampled({});
            wall.state(52u, 0u);
            wall.dual.set_exact_turn(true);
            wall.rt.memory().store32(variables + 0x14u, 1u);
            wall.run_dive_state();
            wall.dual.sampled(wall.mouse(5.0f));
            (void)wall.turn_reads();
            require(close_to(wall.heading(variables + 0x1Cu), 5.0), "a dive off a wall ends like a dive");
        }
        // The exact stick: its rate times the frame is an angle, and both pressures are answered as zero.
        {
            Player p;
            p.dual.set_exact_turn(true);
            p.dual.sampled(p.stick(235u));
            const auto [l, r] = p.turn_reads();
            require(l == 0.0f && r == 0.0f, "the exact stick's turn pressures are zero");
            require(close_to(p.heading(physics_data + 0xB4u), Controls::turn_of(235u) * dt, 1.0e-3), "the stick's rate times the frame, as an angle");
            p.dual.sampled(p.stick(21u));
            (void)p.turn_reads();
            require(close_to(p.heading(physics_data + 0xB4u), 0.0, 1.0e-3), "and back");
            // The script's own look angle starts every frame from zero while the port turns the player.
            p.number(script_data + 0x124u, 1.5f);
            p.dual.sampled({});
            (void)p.turn_reads();
            require(p.number(script_data + 0x124u) == 0.0f, "the look angle the turn pressures feed does not pile up");
        }
        {
            // Off: the game's own rate.
            Player rate;
            rate.dual.sampled(rate.stick(235u));
            const auto [l, r] = rate.turn_reads();
            require(l == 0.0f && close_to(r, Controls::pressure_of(Controls::raw(235u)) * Controls::look_gain, 1.0e-4) && rate.heading(physics_data + 0xB4u) == 0.0f,
                "without ExactTurn the stick is the game's rate");
            rate.number(script_data + 0x124u, 1.5f);
            rate.dual.sampled({});
            (void)rate.turn_reads();
            require(rate.number(script_data + 0x124u) == 1.5f, "and the look angle is the game's");
        }
        // The view's pitch (Camera=tight): the camera's current orientation is pitched by the mouse's angle,
        // as far as the look itself goes (45 degrees), and the level's tilt is taken out at the camera's update.
        {
            Player p;
            p.dual.set_tight_view(true);
            p.dual.sampled(p.mouse(0.0f, 10.0f));
            require(p.pressure(Controls::LookUp, 0x08B755E4u) == 0.0f, "a pitch down is not look_up's");
            require(close_to(p.pressure(Controls::LookDown, look_down_site), std::cbrt(10.0 / dt / 57.29578), 1.0e-4), "the pitch as the pressure whose cube times the frame is the angle");
            require(close_to(p.pitch(camera_data + 0x34u), 10.0) && p.pitch(camera_data + 0x24u) == 0.0f, "the camera's current orientation pitched by the angle, once");
            p.number(script_data + 0x120u, 0.7f);
            p.dual.sampled(p.mouse(0.0f, 10.0f));
            (void)p.pressure(Controls::LookUp, 0x08B755E4u);
            require(close_to(p.pitch(camera_data + 0x34u), 10.0 + (0.7853982 - 0.7) * 57.29578, 2.0e-3), "no further than the look goes");

            p.number(camera_data + 0x190u, -0.4f); p.number(camera_data + 0x194u, -0.2f);
            p.number(other_camera_data + 0x190u, -0.4f);
            auto ctx = p.rt.cpu();
            ctx.gpr[31] = 0u;
            p.rt.memory().store32(current_entity, other_camera);
            p.rt.function_at(camera_update)(p.rt, ctx);
            require(p.number(other_camera_data + 0x190u) == -0.4f && p.number(camera_data + 0x190u) == -0.4f, "another camera keeps its tilt");
            p.rt.memory().store32(current_entity, camera);
            p.rt.function_at(camera_update)(p.rt, ctx);
            require(p.number(camera_data + 0x190u) == 0.0f && p.number(camera_data + 0x194u) == 0.0f, "the player's camera loses the level's tilt");
        }
        {
            Player original;
            original.dual.sampled(original.mouse(0.0f, 10.0f));
            (void)original.pressure(Controls::LookDown, look_down_site);
            require(original.pitch(camera_data + 0x34u) == 0.0f, "Camera=original: the game's own ease");
        }
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
