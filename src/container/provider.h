#pragma once

#include "core/types.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace offcat {

// ── Container Provider interface ────────────────────────────────────

class ContainerProvider {
public:
    virtual ~ContainerProvider() = default;

    virtual std::string type() const = 0;
    virtual bool scan(int64_t container_entry_id,
                      Database& db,
                      const ContainerOptions& options) = 0;
};

// ── Provider Registry ───────────────────────────────────────────────

class ProviderRegistry {
public:
    static ProviderRegistry& instance();

    void register_provider(std::shared_ptr<ContainerProvider> provider);
    std::shared_ptr<ContainerProvider> find_provider(const std::string& type) const;
    std::vector<std::string> registered_types() const;

private:
    ProviderRegistry() = default;
    std::vector<std::shared_ptr<ContainerProvider>> providers_;
};

// ── Virtual Tree Writer ─────────────────────────────────────────────

class VirtualTreeWriter {
public:
    VirtualTreeWriter(Database& db, int64_t source_id,
                      int64_t container_entry_id);

    // Add a virtual entry under the given parent (0 = container root)
    Result<int64_t> add_entry(const EntryData& entry);

    // Get the container's root entry ID
    int64_t container_entry_id() const { return container_entry_id_; }

private:
    Database& db_;
    EntryManager entry_mgr_;
    int64_t source_id_;
    int64_t container_entry_id_;
};

} // namespace offcat
