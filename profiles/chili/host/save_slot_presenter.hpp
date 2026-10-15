#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace chili {
struct SaveSlot {std::string name,display_title;bool exists{};};
// Delete: only a slot that holds a save can be chosen, and never without the
// separate question.
enum class SaveSlotOperation {Save,Load,Delete};
struct SaveSlotRequest {
    std::uint64_t generation{};
    std::string title;
    std::vector<SaveSlot> slots;
    std::size_t initial_index{};
    std::uint32_t language{},button_swap{};
    SaveSlotOperation operation=SaveSlotOperation::Save;
    // One more sentence for the question about an existing slot (what happens
    // to the save that is there). Empty: the presenter's own text alone.
    std::string confirmation_note;
};
enum class SaveSlotChoice {Choose,Save=Choose,Cancel};
struct SaveSlotSelection {
    std::uint64_t generation{};
    SaveSlotChoice choice{};
    std::size_t index{}; // Meaningful only for Choose (Save is an alias).
    // The player answered the separate question about a slot that holds a save:
    // Overwrite for Save, Delete for Delete. A presenter sets it only then. The
    // caller writes over or deletes an existing save only with it, and takes a
    // choice of such a slot without it as a cancellation.
    bool confirmed{};
};
// Pure UI boundary: no guest pointers, disk paths, crypto or save operations.
// Opening/focusing a slot and programmatic destruction are never a choice.
// An existing slot requires a separate explicit overwrite confirmation.
// The caller supplies fresh generation tokens and validated slot metadata.
class SaveSlotPresenter {
public:
    virtual ~SaveSlotPresenter()=default;
    virtual void open(const SaveSlotRequest &)=0;
    virtual std::optional<SaveSlotSelection> poll(std::uint64_t generation)=0;
    virtual void close(std::uint64_t generation) noexcept=0;
};
}
