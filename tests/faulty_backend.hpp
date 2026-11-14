#pragma once

// A storage backend for tests that fails on purpose, as a drive does when it is unplugged or a
// write is cut short: the Nth write fails before anything is stored, or the rename into place
// fails (a temporary file is left), or the Nth pack written is left truncated under its name (a
// write the drive lost half of), or the Nth delete or pack read fails. Everything else goes to
// the real backend below it. (Every real backend replaces a file atomically, so only a pack, never
// yet listed in an index when it is cut short, is truncated here.)

#include <memory>
#include <string>

#include "vaultik/repo/backend.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::test {

class FaultyBackend : public Backend {
public:
    enum class Fault { None, FailWrite, TruncateWrite, FailRename, FailRemove, FailRead };

    FaultyBackend(std::unique_ptr<Backend> inner, Fault fault, int at) : inner_(std::move(inner)), fault_(fault), at_(at) {}

    // Whether the fault happened (later calls go through again: the drive came back).
    bool fired() const { return fired_; }
    int writes() const { return writes_; }

    std::string location() const override { return inner_->location(); }
    bool exists(const std::string& name) override { return inner_->exists(name); }
    bool exists_now(const std::string& name) override { return inner_->exists_now(name); }

    void put(const std::string& name, ByteSpan data) override { write(name, data, false); }
    void put_data(const std::string& name, ByteSpan data) override { write(name, data, true); }

    Bytes get(const std::string& name) override {
        if (name.starts_with("data/") && hit(Fault::FailRead, reads_)) throw Error("simulated read failure of " + name + ": the device is not ready");
        return inner_->get(name);
    }
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override {
        if (name.starts_with("data/") && hit(Fault::FailRead, reads_)) throw Error("simulated read failure of " + name + ": the device is not ready");
        return inner_->get_range(name, offset, length);
    }
    std::uint64_t size(const std::string& name) override { return inner_->size(name); }
    std::vector<std::string> list(const std::string& dir) override { return inner_->list(dir); }
    void remove(const std::string& name) override {
        if (hit(Fault::FailRemove, removes_)) throw Error("simulated failure to delete " + name);
        inner_->remove(name);
    }
    std::string data_class() const override { return inner_->data_class(); }
    bool data_needs_thaw() const override { return inner_->data_needs_thaw(); }

private:
    // Counts the calls of a kind; true (once) on the one that fails.
    bool hit(Fault kind, int& count) {
        if (fault_ != kind || fired_) return false;
        if (++count != at_) return false;
        fired_ = true;
        return true;
    }

    void write(const std::string& name, ByteSpan data, bool as_data) {
        // Locks are not counted: they come and go around every run. Only packs are cut short.
        const bool counted = fault_ == Fault::TruncateWrite ? name.starts_with("data/") : !name.starts_with("locks/");
        if (counted && (fault_ == Fault::FailWrite || fault_ == Fault::TruncateWrite || fault_ == Fault::FailRename) &&
            !fired_ && ++writes_ == at_) {
            fired_ = true;
            if (fault_ == Fault::TruncateWrite) {
                // Half of it under its own name, as a replace that is not atomic leaves it.
                const ByteSpan half = data.subspan(0, data.size() / 2);
                if (as_data) inner_->put_data(name, half);
                else inner_->put(name, half);
            } else if (fault_ == Fault::FailRename) {
                // The whole file, but under a temporary name that never got renamed.
                inner_->put(name + ".tmp-0123456789abcdef", data);
            }
            throw Error("simulated write failure of " + name + ": the drive was removed");
        }
        if (as_data) inner_->put_data(name, data);
        else inner_->put(name, data);
    }

    std::unique_ptr<Backend> inner_;
    Fault fault_;
    int at_;
    bool fired_ = false;
    int writes_ = 0;
    int reads_ = 0;
    int removes_ = 0;
};

}  // namespace vaultik::test
