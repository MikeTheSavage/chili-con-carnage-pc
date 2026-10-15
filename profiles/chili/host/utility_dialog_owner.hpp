#pragma once
namespace chili {
enum class UtilityDialogType { None, Savedata, Message, Netconf };
// PSP utilities share one active dialog slot. The last selected type remains
// observable to GetStatus after teardown, until another dialog initializes.
class UtilityDialogOwner {
    UtilityDialogType current_{UtilityDialogType::None};
    bool busy_{};
public:
    bool try_acquire(UtilityDialogType type) {
        if (busy_ || type == UtilityDialogType::None) return false;
        current_ = type;
        busy_ = true;
        return true;
    }
    bool release(UtilityDialogType type) {
        if (!busy_ || current_ != type) return false;
        busy_ = false;
        return true;
    }
    [[nodiscard]] bool is_current_type(UtilityDialogType type) const {
        return current_ == type;
    }
    [[nodiscard]] bool busy() const { return busy_; }
};
} // namespace chili
