#include "native_savedata_backend.hpp"
#include "native_save_codec.hpp"
#include "param_sfo.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <utility>

namespace {
namespace fs = std::filesystem;
namespace storage = chili::savedata_storage;
namespace codec = chili::native_save;
namespace sfo = chili::sfo;
using Bytes = std::vector<std::uint8_t>;
std::size_t checks{};
void require(bool b, const char* message) { ++checks; if (!b) throw std::runtime_error(message); }
bool plain_directory_chain(const fs::path& path) {
    if (!path.is_absolute()) return false;
    auto current = path.root_path();
    auto valid = [](const fs::path& p) {
        const auto flags = GetFileAttributesW(p.c_str());
        return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) &&
               !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
    };
    if (!valid(current)) return false;
    for (const auto& part : path.relative_path()) {
        current /= part;
        if (!valid(current)) return false;
    }
    return true;
}
Bytes read_fixture(const fs::path& path, std::size_t cap) {
    require(fs::file_size(path) <= cap, "fixture input exceeds its bound");
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot open fixture input");
    return {std::istreambuf_iterator<char>(file), {}};
}
void write_fixture(const fs::path& path, const Bytes& bytes) {
    require(plain_directory_chain(path.parent_path()), "fixture artifact parent is unsafe");
    const auto flags = GetFileAttributesW(path.c_str());
    require(flags == INVALID_FILE_ATTRIBUTES || !(flags & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)),
            "fixture artifact path is not a plain file");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(file), "cannot write owned fixture artifact");
}
template<class F> void rejects(F&& f, std::uint32_t result, const char* message) {
    try { f(); }
    catch (const chili::SavedataBackendError& e) {
        if (e.result != result) throw std::runtime_error(std::string(message) + "; expected=" + std::to_string(result) + "; observed=" + std::to_string(e.result));
        require(true, message); return;
    }
    throw std::runtime_error(message);
}
struct Root {
    fs::path parent, path;
    explicit Root(const fs::path& prep) {
        require(plain_directory_chain(prep), "fixture preparation root contains a reparse path");
        parent = prep / "fixture_roots";
        fs::create_directory(parent);
        require(plain_directory_chain(parent), "fixture temporary root parent is unsafe");
        path = parent / ("case_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        require(fs::create_directory(path), "temporary test root collision");
    }
    ~Root() {
        // All recursive removal is confined to this exclusively created fixture root.
        // No reparse entries are created by this fixture.
        try {
            if (!path.is_absolute() || path.parent_path() != parent ||
                !path.filename().string().starts_with("case_") || !plain_directory_chain(path) ||
                fs::weakly_canonical(path).parent_path() != fs::weakly_canonical(parent)) return;
            std::size_t entries{};
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (++entries > 4096) return;
                const auto flags = GetFileAttributesW(entry.path().c_str());
                if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return;
            }
            std::error_code error;
            fs::remove_all(path, error);
        } catch (...) { /* preserve an unexpected/unsafe fixture tree */ }
    }
};
struct Fixture {
    psprecomp::GuestMemory memory;
    chili::save_request::Request request;
    Bytes payload;
    std::array<Bytes, 4> media;
    explicit Fixture(const fs::path& captures) {
        const auto ram = read_fixture(captures / "listsave_pointer_actual_request_ram.bin", 65536);
        require(ram.size() == 65536, "actual request RAM must be 64KiB");
        constexpr std::uint32_t params = 0x08df68c0u;
        memory.copy_in(0x08df0000u, ram);
        payload = read_fixture(captures / "listsave_pointer_actual_payload.bin", 1066);
        require(payload.size() == 1066 && memory.load32(params + 116) == 0x09cb35c0,
                "actual pointer-followed payload mismatch");
        memory.copy_in(memory.load32(params + 116), payload);
        constexpr std::array<const char*, 4> inputs{"icon0", nullptr, "pic1", "snd0"};
        for (std::size_t i = 0; i < inputs.size(); ++i) if (inputs[i]) {
            media[i] = read_fixture(captures / (std::string("listsave_pointer_actual_") + inputs[i] + ".bin"), 4u * 1024u * 1024u);
            const auto at = params + 1412 + static_cast<std::uint32_t>(16 * i);
            require(media[i].size() == memory.load32(at + 8), "actual media length mismatch");
            memory.copy_in(memory.load32(at), media[i]);
        }
        // Original static text at a separately verified pointer, absent from the RAM slice.
        const auto title = memory.load32(memory.load32(params + 1476) + 16);
        const Bytes label{'N','e','w',' ','d','a','t','a',0};
        memory.copy_in(title, label);
        request = chili::save_request::parse(memory, params);
        require(request.game == "ULUS10216" && request.file == "SECURE.BIN" && request.mode == 5 &&
                request.secure_version == 0 && request.slots.size() == 20 && request.data.size == 1066 &&
                request.title == "RAM" && request.savedata_title == "RAM" && request.detail == "0%" && request.parental == 9,
                "actual request interpretation mismatch");
    }
    Bytes snapshot() const {
        // Private guest bytes are compared only in memory, never logged/artifacted.
        Bytes result(65536);
        memory.copy_out(0x08df0000u, result);
        auto append = [&](const chili::save_request::Buffer& b) {
            if (b.capacity) {
                const auto at = result.size();
                result.resize(at + b.capacity);
                memory.copy_out(b.address, std::span(result).subspan(at, b.capacity));
            }
        };
        append(request.data);
        for (const auto& b : request.media) append(b);
        return result;
    }
};
std::vector<storage::File> read_files(storage::Store& store, const storage::Slot& slot) {
    const auto guard = store.guard();
    std::vector<storage::File> result;
    for (const auto& file : store.query(slot).files) result.push_back({file.name, store.read(slot, file.name, 32u * 1024u * 1024u)});
    return result;
}
bool same_files(const std::vector<storage::File>& a, const std::vector<storage::File>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.name == y.name && x.bytes == y.bytes; });
}
void restore(storage::Store& store, const storage::Slot& slot, const std::vector<storage::File>& files) {
    const auto result = store.commit(slot, files, storage::Overwrite::ReplaceWithRollback);
    require(result.outcome == storage::Outcome::Committed, "fixture restore failed");
}
storage::File& named(std::vector<storage::File>& files, const char* name) {
    const auto i = std::find_if(files.begin(), files.end(), [&](const auto& f) { return f.name == name; });
    if (i == files.end()) throw std::runtime_error("fixture managed file absent");
    return *i;
}
void basic_and_corruption_tests(const fs::path& prep, Fixture& fixture, Root& root) {
    chili::NativeSavedataBackend backend(fixture.memory, root.path);
    storage::Store store(root.path, {}, backend.serialization());
    const auto& request = fixture.request;
    auto load_request = request;
    load_request.mode = 4;
    const auto before = fixture.snapshot();
    auto slots = backend.inspect(request);
    require(slots.size() == 20, "request slot count not preserved");
    for (std::size_t i = 0; i < slots.size(); ++i)
        require(slots[i].slot.name == request.slots[i] && !slots[i].slot.exists &&
                slots[i].slot.display_title == "New data" && slots[i].written_filetime == 0,
                "missing slot order/title/presence wrong");
    require(!fs::exists(root.path / "PSP"), "inspect created a save directory");
    for (const auto* file : {"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3", "OTHER.BIN"}) {
        auto bad = request; bad.file = file;
        rejects([&] { backend.save(bad, "DATA00", false); }, 0x80110388, "non-profile filename accepted");
    }
    for (const auto* game : {"C", "AB", "A", "OTHER_GAME"}) {
        auto bad = request; bad.game = game;
        rejects([&] { (void)backend.inspect(bad); }, 0x80110388, "ambiguous/foreign game accepted");
    }
    auto bad_version = request; bad_version.secure_version = 1;
    rejects([&] { backend.save(bad_version, "DATA00", false); }, 0x80110388, "unadopted secureVersion accepted");
    rejects([&] { backend.save(request, "DATA99", false); }, 0x80110388, "unrequested slot accepted");
    require(!fs::exists(root.path / "PSP"), "invalid policy request performed IO");

    backend.save(request, "DATA00", false);
    require(backend.last_commit_result()->outcome == storage::Outcome::Committed, "real new-slot commit absent");
    const storage::Slot slot{request.game, "DATA00"};
    const auto original = read_files(store, slot);
    require(original.size() == 5, "payload/SFO/three actual media set incomplete");
    const auto raw_sfo = store.read(slot, "PARAM.SFO", codec::max_sfo_bytes);
    const auto container = store.read(slot, request.file, 1130);
    require(container.size() == 1130 && codec::decode(container, raw_sfo, request.key) == fixture.payload,
            "actual native disk container does not authenticate exact SFO/payload");
    const auto doc = sfo::parse(raw_sfo);
    require(doc.entries.size() == 7 && doc.find("TITLE")->storage.size() == 128 &&
            doc.find("SAVEDATA_TITLE")->storage.size() == 128 && doc.find("SAVEDATA_DETAIL")->storage.size() == 1024 &&
            doc.find("SAVEDATA_DIRECTORY")->text_value() == "ULUS10216DATA00" &&
            doc.find("CHILI_PC_FORMAT")->integer_value() == 1 && doc.find("CHILI_PC_FILE")->text_value() == "SECURE.BIN" &&
            !doc.find("SAVEDATA_PARAMS") && !doc.find("SAVEDATA_FILE_LIST"), "native SFO format/capacities wrong");
    // Encrypted + public metadata artifacts only, for independent Python verification.
    write_fixture(prep / "fixture_native_PARAM.SFO", raw_sfo);
    write_fixture(prep / "fixture_native_container.bin", container);
    slots = backend.inspect(request);
    require(slots[0].slot.exists && slots[0].slot.display_title == "RAM" && slots[0].written_filetime != 0,
            "actual slot metadata missing");
    auto order = request; std::reverse(order.slots.begin(), order.slots.end());
    require(backend.inspect(order).front().slot.name == "DATA19" && backend.inspect(order).back().slot.name == "DATA00",
            "inspect reordered caller slots");
    const auto loaded = backend.load(load_request, "DATA00");
    require(loaded.payload == fixture.payload && loaded.media == fixture.media && loaded.title == "RAM" &&
            loaded.savedata_title == "RAM" && loaded.detail == "0%" && loaded.parental == 9,
            "actual payload/media/metadata load differs");
    require(fixture.snapshot() == before, "backend operation changed guest memory");
    auto auto_request = load_request; auto_request.mode = 0; auto_request.slot = "DATA00";
    require(backend.inspect(auto_request).size() == 1 && backend.load(auto_request, "DATA00").payload == fixture.payload,
            "automatic load does not share native path");
    rejects([&] { (void)backend.load(load_request, "DATA01"); }, 0x80110307, "missing slot not mapped NO_DATA");
    rejects([&] { backend.save(request, "DATA00", false); }, 0x80110385, "unexpected existing slot overwritten");
    require(same_files(read_files(store, slot), original), "overwrite rejection changed old files");
    // This check once asserted the opposite: a request whose word at
    // offset 56 is 0 was refused with 0x80110388 when asked to replace a save
    // ("wire-forbidden overwrite"). The word is no permission: the game's own
    // automatic save carries 0 there and a PSP replaces the save all the same.
    // Read as a permission it made every save after a profile's first one fail.
    // The caller's replace_existing alone decides now; without it the slot is
    // still refused (the line above).
    {   auto word_zero = request; word_zero.overwrite_word = 0;
        const auto report = backend.save(word_zero, "DATA00", true);
        require(report.replaced && backend.last_commit_result()->outcome == storage::Outcome::Committed &&
                backend.load(load_request, "DATA00").payload == fixture.payload,
                "a replacement the caller asked for was refused because of the request's word at offset 56");
        restore(store, slot, original); }
    auto short_payload = load_request; --short_payload.data.capacity; short_payload.data.size = 0;
    rejects([&] { (void)backend.load(short_payload, "DATA00"); }, 0x80110308, "undersized plaintext output accepted");
    auto small_media = load_request; --small_media.media[0].capacity; small_media.media[0].size = 0;
    rejects([&] { (void)backend.load(small_media, "DATA00"); }, 0x80110308, "undersized media output accepted");
    auto skip_media = load_request; skip_media.media = {};
    const auto without_media = backend.load(skip_media, "DATA00");
    require(without_media.payload == fixture.payload && std::all_of(without_media.media.begin(), without_media.media.end(),
            [](const auto& bytes) { return bytes.empty(); }), "zero-cap optional media was loaded");
    auto optional = load_request; optional.media[1] = {0x08920000, 128, 0};
    require(backend.load(optional, "DATA00").media[1].empty(), "absent optional media fabricated");
    auto wrong_key = load_request; wrong_key.key[0] ^= 0x80;
    rejects([&] { (void)backend.load(wrong_key, "DATA00"); }, 0x80110306, "wrong key not mapped DATA_BROKEN");

    // Save uses bytes at the actual action, not a cached Init payload snapshot.
    const auto original_first = fixture.memory.load8(request.data.address);
    fixture.memory.store8(request.data.address, static_cast<std::uint8_t>(original_first ^ 0x20));
    backend.save(request, "DATA00", true);
    auto updated = backend.load(load_request, "DATA00").payload;
    require(updated[0] == static_cast<std::uint8_t>(original_first ^ 0x20) && updated.size() == 1066,
            "save did not capture current guest payload");
    fixture.memory.store8(request.data.address, original_first);
    restore(store, slot, original);

    for (const auto* name : {"PARAM.SFO", "SECURE.BIN"}) {
        auto absent = original;
        absent.erase(std::remove_if(absent.begin(), absent.end(), [&](const auto& f) { return f.name == name; }), absent.end());
        restore(store, slot, absent);
        rejects([&] { (void)backend.load(load_request, "DATA00"); },
                std::string_view(name) == "PARAM.SFO" ? 0x80110306 : 0x80110309, "required missing file wrong result");
        restore(store, slot, original);
    }
    for (std::size_t at : {std::size_t(32), std::size_t(48 + 27), container.size() - 1}) {
        auto bad = original; named(bad, "SECURE.BIN").bytes[at] ^= 1; restore(store, slot, bad);
        rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306, "native nonce/cipher/tag tamper accepted");
        restore(store, slot, original);
    }
    { auto bad = original; named(bad, "SECURE.BIN").bytes.pop_back(); restore(store, slot, bad);
      rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306, "truncated native container accepted"); restore(store, slot, original); }
    { auto bad = original; named(bad, "SECURE.BIN").bytes.assign(1082, 0);
      restore(store, slot, bad); rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306,
      "PSP-style unknown container accepted"); restore(store, slot, original); }
    { auto bad = original; auto metadata = doc;
      *metadata.find("SAVEDATA_TITLE") = sfo::Entry::text("SAVEDATA_TITLE", "Tampered title", 128);
      named(bad, "PARAM.SFO").bytes = sfo::serialize(metadata); restore(store, slot, bad);
      require(backend.inspect(request)[0].slot.display_title == "Tampered title", "valid foreign display title hidden");
      rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306, "changed SFO accepted without authentication");
      restore(store, slot, original); }
    for (const auto* key : {"SAVEDATA_DIRECTORY", "CHILI_PC_FILE", "TITLE", "PARENTAL_LEVEL", "CHILI_PC_FORMAT"}) {
        auto bad = original; auto metadata = doc;
        if (std::string_view(key) == "SAVEDATA_DIRECTORY") *metadata.find(key) = sfo::Entry::text(key, "ULUS10216DATA01", 32);
        else if (std::string_view(key) == "CHILI_PC_FILE") *metadata.find(key) = sfo::Entry::text(key, "OTHER.BIN", 13);
        else if (std::string_view(key) == "TITLE") *metadata.find(key) = sfo::Entry::text(key, "RAM", 129);
        else *metadata.find(key) = sfo::Entry::integer(key, 300);
        auto& metadata_bytes = named(bad, "PARAM.SFO").bytes; metadata_bytes = sfo::serialize(metadata);
        named(bad, "SECURE.BIN").bytes = codec::encode(fixture.payload, metadata_bytes, request.key);
        restore(store, slot, bad);
        rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306, "authenticated invalid native metadata accepted");
        restore(store, slot, original);
    }
    { auto bad = original; named(bad, "PARAM.SFO").bytes = {0}; restore(store, slot, bad);
      require(backend.inspect(request)[0].slot.exists, "malformed metadata slot presence hidden");
      rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306, "malformed SFO accepted"); restore(store, slot, original); }
    { auto bad = original; named(bad, "PARAM.SFO").bytes.resize(codec::max_sfo_bytes + 1);
      restore(store, slot, bad); rejects([&] { (void)backend.load(load_request, "DATA00"); }, 0x80110306,
      "oversized SFO not rejected as broken metadata"); restore(store, slot, original); }
    { const auto foreign = sfo::serialize(sfo::Document{{sfo::Entry::text("TITLE", "Existing PSP title", 128)}});
      auto files = original; named(files, "PARAM.SFO").bytes = foreign; restore(store, {request.game, "DATA03"}, files);
      require(backend.inspect(request)[3].slot.exists && backend.inspect(request)[3].slot.display_title == "Existing PSP title",
              "foreign valid title cannot be inspected");
      rejects([&] { (void)backend.load(load_request, "DATA03"); }, 0x80110306, "foreign SFO loaded as native"); }
    auto malformed_save = request; malformed_save.title.assign(128, 'A');
    rejects([&] { backend.save(malformed_save, "DATA07", false); }, 0x80110388, "oversized source title encoded");
    malformed_save = request; malformed_save.data.address = 0x04000000;
    rejects([&] { backend.save(malformed_save, "DATA07", false); }, 0x80110388, "unsupported guest source bank accepted");
    require(!store.query({request.game, "DATA07"}).exists && fixture.snapshot() == before,
            "failed save/load changed guest or made a partial slot");
    require(same_files(read_files(store, slot), original), "corruption fixture did not restore original generation");
}

Bytes read_plain(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot open a file of a kept save");
    return {std::istreambuf_iterator<char>(file), {}};
}
std::size_t transactions(const Root& root) {
    std::size_t count{};
    for (const auto& entry : fs::directory_iterator(root.path / "PSP" / "SAVEDATA"))
        if (entry.path().filename().string().starts_with("._CHILI_TXN_")) ++count;
    return count;
}
// The generation a save replaces is kept beside SAVEDATA, a failed keep never
// fails the save, an interrupted save is finished at the next operation, and a
// delete moves the slot's folder to the same kept place.
void kept_generation_and_delete_tests(Fixture& fixture, Root& root) {
    std::vector<std::string> lines;
    chili::NativeSavedataBackend backend(fixture.memory, root.path, {}, {}, {}, [&](const std::string& line) { lines.push_back(line); });
    storage::Store store(root.path, {}, backend.serialization());
    auto request = fixture.request; request.overwrite_word = 0;
    auto load_request = request; load_request.mode = 4;
    const storage::Slot slot{request.game, "DATA12"};
    const auto kept = root.path / "PSP" / "SAVEDATA_PREVIOUS" / "ULUS10216DATA12";
    auto decoded = [&](const fs::path& folder) { return codec::decode(read_plain(folder / "SECURE.BIN"), read_plain(folder / "PARAM.SFO"), request.key); };
    auto payload_with = [&](std::uint8_t first) { auto bytes = fixture.payload; bytes[0] = first; return bytes; };
    auto save_with = [&](std::uint8_t first, bool replace) {
        fixture.memory.store8(request.data.address, first);
        auto report = backend.save(request, slot.save, replace);
        fixture.memory.store8(request.data.address, fixture.payload[0]);
        return report; };
    require(backend.previous_place() == "PSP\\SAVEDATA_PREVIOUS", "the kept place is not named for the dialog");
    auto report = save_with(0xA1, true);
    require(!report.replaced && report.previous.empty() && report.note.empty() && !fs::exists(kept), "a save into an empty slot reported or kept an earlier save");
    report = save_with(0xA2, true);
    require(report.replaced && fs::path(report.previous) == kept && report.note.empty(), "a replacement did not report where the earlier save is kept");
    require(decoded(kept) == payload_with(0xA1) && backend.load(load_request, slot.save).payload == payload_with(0xA2), "the kept earlier save is not the data from before the save");
    for (const auto* media : {"ICON0.PNG", "PIC1.PNG", "SND0.AT3"}) require(fs::exists(kept / media), "the kept earlier save lost a media file");
    report = save_with(0xA3, true);
    require(report.replaced && decoded(kept) == payload_with(0xA2) && transactions(root) == 0, "the kept earlier save was not replaced at the next save, or a transaction was left");
    // The earlier save cannot be kept at the moment of the save (injected at the
    // step that keeps it): the save is committed anyway and says so; the old
    // generation waits whole in its work folder and is kept at the next operation.
    backend.set_fault_hook_for_test([](storage::Phase p) { if (p == storage::Phase::BeforeKeep) throw std::runtime_error("fixture keep failure"); });
    report = save_with(0xA4, true);
    backend.set_fault_hook_for_test({});
    require(report.replaced && report.previous.empty() && report.note.find("could not be put in the kept place now") != std::string::npos &&
            backend.last_commit_result()->outcome == storage::Outcome::CommittedCleanupRequired && backend.last_commit_result()->previous_failure,
            "a save whose earlier generation could not be kept failed or hid the reason");
    require(decoded(kept) == payload_with(0xA2) && transactions(root) == 1, "a failed keep destroyed the earlier kept save or lost the generation it could not keep");
    lines.clear();
    require(backend.load(load_request, slot.save).payload == payload_with(0xA4) && decoded(kept) == payload_with(0xA3) && transactions(root) == 0,
            "the generation a save could not keep was not kept at the next operation");
    require(lines.size() == 1 && lines[0].find("is now the earlier save kept in") != std::string::npos, "keeping a waiting generation was not logged once");
    // The kept place itself is unusable (a folder inside it that this program did
    // not put there): saves go on working, the kept folder is left as it is, and
    // nothing piles up in SAVEDATA.
    fs::create_directory(kept / "not ours");
    for (const std::uint8_t first : {std::uint8_t{0xB1}, std::uint8_t{0xB2}, std::uint8_t{0xB3}}) {
        report = save_with(first, true);
        require(report.replaced && report.previous.empty() && !report.note.empty(), "a save with an unusable kept place did not say so");
        require(backend.load(load_request, slot.save).payload == payload_with(first) && transactions(root) == 0 && fs::is_directory(kept / "not ours"),
                "a save with an unusable kept place failed, left work folders or emptied the kept folder");
    }
    fs::remove(kept / "not ours");
    report = save_with(0xA4, true);
    require(report.replaced && fs::path(report.previous) == kept && decoded(kept) == payload_with(0xB3), "keeping did not resume once the kept place was usable again");
    // A save that ended between moving the old generation away and publishing the
    // new one (made by hand here): the next operation puts the old one back.
    const auto slot_dir = root.path / "PSP" / "SAVEDATA" / "ULUS10216DATA12";
    const auto interrupted = root.path / "PSP" / "SAVEDATA" / "._CHILI_TXN_1_1_1_ULUS10216DATA12";
    fs::create_directory(interrupted); fs::rename(slot_dir, interrupted / "old"); fs::create_directory(interrupted / "new");
    { std::ofstream half(interrupted / "new" / "PARAM.SFO", std::ios::binary); half << "half"; }
    lines.clear();
    const auto slots = backend.inspect(load_request);
    require(slots[12].slot.exists && !fs::exists(interrupted) && backend.load(load_request, slot.save).payload == payload_with(0xA4),
            "a save interrupted after its old generation was moved away was not put back");
    require(lines.size() == 1 && lines[0].find("it is back in") != std::string::npos, "putting an interrupted save back was not logged once");
    // Mode 7: the slot leaves the list, its folder becomes the kept earlier save.
    auto delete_request = request; delete_request.mode = 7; delete_request.data = {}; delete_request.media = {};
    rejects([&] { (void)backend.remove(request, slot.save); }, 0x80110348, "a save request deleted a slot");
    rejects([&] { (void)backend.remove(delete_request, "DATA99"); }, 0x80110348, "a delete of a slot outside the list was accepted");
    require(backend.inspect(delete_request)[12].slot.exists, "the delete list does not show the save");
    const auto moved = backend.remove(delete_request, slot.save);
    require(fs::path(moved) == kept && !fs::exists(slot_dir) && !store.query(slot).exists && !backend.inspect(delete_request)[12].slot.exists,
            "a deleted slot is still in the list");
    require(decoded(kept) == payload_with(0xA4), "a deleted save was erased instead of kept");
    rejects([&] { (void)backend.remove(delete_request, slot.save); }, 0x80110347, "a delete of an empty slot is not NO_DATA");
    rejects([&] { (void)backend.load(load_request, slot.save); }, 0x80110307, "a deleted slot still loads");
    report = save_with(0xA5, false);
    require(!report.replaced && decoded(kept) == payload_with(0xA4), "a save into the freed slot failed or touched the deleted save");
}

fs::path active_transaction(const Root& root) {
    for (const auto& entry : fs::directory_iterator(root.path / "PSP" / "SAVEDATA"))
        if (entry.path().filename().string().starts_with("._CHILI_TXN_") && fs::exists(entry.path() / "old")) return entry.path();
    throw std::runtime_error("fixture transaction backup absent");
}
void commit_failure_tests(Fixture& fixture, Root& root) {
    std::size_t diagnostics{};
    chili::NativeSavedataBackend backend(fixture.memory, root.path,
        [&](const storage::CommitResult& result) { require(result.outcome == storage::Outcome::CommittedCleanupRequired,
            "cleanup diagnostic wrong state"); ++diagnostics; });
    storage::Store store(root.path, {}, backend.serialization());
    const auto& request = fixture.request;
    const storage::Slot slot{request.game, "DATA10"};
    backend.save(request, slot.save, false);
    const auto original = read_files(store, slot);
    backend.set_fault_hook_for_test([](storage::Phase p) { if (p == storage::Phase::StageComplete) throw std::runtime_error("fixture staged failure"); });
    rejects([&] { backend.save(request, slot.save, true); }, 0x8011038b, "failed staging claimed save success");
    require(backend.last_commit_result()->outcome == storage::Outcome::FailedUnchanged && same_files(read_files(store, slot), original),
            "failed staging did not preserve old SFO/container pair");
    backend.set_fault_hook_for_test({});
    const auto target = root.path / "PSP" / "SAVEDATA" / (slot.game + slot.save);
    HANDLE blocked = CreateFileW(target.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(blocked != INVALID_HANDLE_VALUE, "cannot lock owned fixture directory");
    try {
        bool denied{};
        try { backend.save(request, slot.save, true); }
        catch (const chili::SavedataBackendError& e) {
            denied = true;
            const auto failure = backend.last_commit_result()->failure;
            require(failure && failure->code == storage::ErrorCode::NativeIo &&
                    ((failure->native_error == ERROR_ACCESS_DENIED && e.result == 0x80110384) ||
                     (failure->native_error == ERROR_SHARING_VIOLATION && e.result == 0x80110385)),
                    "real OS denied rename has incoherent concrete guest error mapping");
        }
        require(denied, "real OS denied rename claimed save success");
    }
    catch (...) { CloseHandle(blocked); throw; }
    CloseHandle(blocked);
    require(same_files(read_files(store, slot), original), "real OS denied rename changed old pair");

    // Real Windows backup deletion failure after publication: success must stay committed.
    HANDLE backup_handle = INVALID_HANDLE_VALUE;
    backend.set_fault_hook_for_test([&](storage::Phase phase) {
        if (phase == storage::Phase::BeforeCleanup) {
            const auto file = active_transaction(root) / "old" / "SECURE.BIN";
            backup_handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(backup_handle != INVALID_HANDLE_VALUE, "cannot lock owned backup file");
        }
    });
    try { backend.save(request, slot.save, true); }
    catch (...) { if (backup_handle != INVALID_HANDLE_VALUE) CloseHandle(backup_handle); throw; }
    CloseHandle(backup_handle);
    const auto cleanup = backend.last_commit_result();
    require(cleanup && cleanup->outcome == storage::Outcome::CommittedCleanupRequired && diagnostics == 1 &&
            fs::exists(cleanup->backup_directory / "SECURE.BIN") && store.query(slot).exists,
            "committed cleanup failure lost success/diagnostic/backup");
    backend.set_fault_hook_for_test({});
    // What that save left behind (the old generation, in its transaction folder)
    // is dealt with at the next operation: it becomes the kept earlier save.
    require(transactions(root) == 1, "the cleanup failure left no transaction folder to finish");
    (void)backend.inspect(request);
    const auto kept_after_cleanup = root.path / "PSP" / "SAVEDATA_PREVIOUS" / (slot.game + slot.save);
    require(transactions(root) == 0 && store.query(slot).exists, "the leftover of a committed save was not finished at the next operation");
    for (const auto& file : original)
        require(read_plain(kept_after_cleanup / file.name) == file.bytes, "the generation a committed save could not keep at once was not kept whole afterwards");

    backend.save(request, "DATA11", false);
    backend.set_fault_hook_for_test([](storage::Phase p) {
        if (p == storage::Phase::Published || p == storage::Phase::BeforeRestore)
            throw std::runtime_error("fixture rollback failure");
    });
    bool recovery_thrown{};
    try { backend.save(request, "DATA11", true); }
    catch (const chili::NativeSavedataRecoveryError& e) {
        recovery_thrown = true;
        require(e.result == 0x8011038b && e.recovery().outcome == storage::Outcome::RecoveryRequired &&
                !e.recovery().recovery_directory.empty() && fs::exists(e.recovery().backup_directory / "PARAM.SFO") &&
                std::string_view(e.what()).find("transaction=") != std::string_view::npos,
                "recovery exception lost explicit status/preserved paths");
    }
    require(recovery_thrown && !store.query({request.game, "DATA11"}).exists,
            "unknown recovery state claimed save success");
    backend.set_fault_hook_for_test({});
    using E = storage::ErrorCode;
    require(chili::NativeSavedataBackend::storage_result_for_test(E::NativeIo, ERROR_DISK_FULL, true) == 0x80110383 &&
            chili::NativeSavedataBackend::storage_result_for_test(E::NativeIo, ERROR_HANDLE_DISK_FULL, true) == 0x80110383 &&
            chili::NativeSavedataBackend::storage_result_for_test(E::NativeIo, ERROR_ACCESS_DENIED, true) == 0x80110384 &&
            chili::NativeSavedataBackend::storage_result_for_test(E::NativeIo, ERROR_WRITE_PROTECT, true) == 0x80110384 &&
            chili::NativeSavedataBackend::storage_result_for_test(E::NativeIo, ERROR_ACCESS_DENIED, false) == 0x80110305,
            "documented concrete native error mapping wrong");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected owned prep root and captures directory");
        const auto prep = fs::weakly_canonical(fs::absolute(argv[1]));
        const auto source_parent = fs::weakly_canonical(fs::path(__FILE__).parent_path());
        const auto official_root = (source_parent.parent_path() / "analysis" / "chili-native-savedata-backend-fixtures").lexically_normal();
        require((source_parent.filename() == "chili_native_savedata_backend_prep" && prep == source_parent) ||
                (source_parent.filename() == "tools" && prep == official_root),
                "fixture root escaped owned prep or official analysis fixture root");
        Fixture fixture(fs::absolute(argv[2]));
        Root root(prep);
        basic_and_corruption_tests(prep, fixture, root);
        commit_failure_tests(fixture, root);
        kept_generation_and_delete_tests(fixture, root);
        std::cout << "PASS: " << checks << " native backend checks; actual captured 1,066-byte payload and media; "
                     "SFO/AES/native disk/new/overwrite/load/tamper/caps/real OS failure/recovery; no real profiles or key logging\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
