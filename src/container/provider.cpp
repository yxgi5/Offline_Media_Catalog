#include "container/provider.h"
#include "core/logger.h"

namespace offcat {

// ── ProviderRegistry ────────────────────────────────────────────────

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry registry;
    return registry;
}

void ProviderRegistry::register_provider(
    std::shared_ptr<ContainerProvider> provider) {
    if (provider) {
        providers_.push_back(std::move(provider));
        LOG_VERBOSE("Registered container provider: " +
                    providers_.back()->type());
    }
}

std::shared_ptr<ContainerProvider> ProviderRegistry::find_provider(
    const std::string& type) const {
    for (const auto& p : providers_) {
        if (p->type() == type) return p;
    }
    return nullptr;
}

std::vector<std::string> ProviderRegistry::registered_types() const {
    std::vector<std::string> types;
    for (const auto& p : providers_) {
        types.push_back(p->type());
    }
    return types;
}

// ── VirtualTreeWriter ───────────────────────────────────────────────

VirtualTreeWriter::VirtualTreeWriter(Database& db, int64_t source_id,
                                       int64_t container_entry_id)
    : db_(db), entry_mgr_(db), source_id_(source_id),
      container_entry_id_(container_entry_id) {}

Result<int64_t> VirtualTreeWriter::add_entry(const EntryData& entry) {
    EntryData virtual_entry = entry;
    virtual_entry.source_id = source_id_;
    virtual_entry.is_virtual = true;

    auto result = entry_mgr_.insert(virtual_entry);
    if (is_err(result)) {
        return result;
    }

    int64_t new_id = get_ok(result);

    // Also add to FTS
    auto path_result = entry_mgr_.build_path(new_id);
    std::string path = is_ok(path_result) ? get_ok(path_result) : entry.name;

    Statement stmt(db_, "SELECT name FROM source WHERE id=?");
    std::string source_name;
    if (stmt.is_valid()) {
        stmt.bind_int64(1, source_id_);
        if (stmt.step()) {
            source_name = stmt.column_text(0);
        }
    }

    auto fts_result = entry_mgr_.insert_fts(new_id, entry.name, path,
                                           source_name);
    if (is_err(fts_result)) {
        LOG_WARN("Failed to index virtual entry: " + entry.name);
    }
    return new_id;
}

} // namespace offcat
