#include "telemetry_registry.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <exception>
#include <utility>

namespace cas {

TelemetryRegistry::TelemetryRegistry(DrainCallback drain)
    : drain_(std::move(drain)) {}

TelemetryRegistry::~TelemetryRegistry() {
    stop_all();
}

void TelemetryRegistry::add(std::unique_ptr<TelemetryBackend> b) {
    if (!b) {
        return;
    }

    std::lock_guard<std::mutex> lock(mu_);
    // Contract: add() must be called BEFORE start_all(). The registry only
    // starts backends during start_all(); a backend added later would never
    // be started, silently dropping its events. Reject such calls loudly so
    // the misuse is caught in development. Builds without NDEBUG (release)
    // get the stderr warning + early return so a faulty caller cannot
    // accidentally inject a never-started backend.
    if (start_attempted_) {
        std::fprintf(stderr,
                     "telemetry: TelemetryRegistry::add(\"%s\") called after "
                     "start_all(); rejecting — add() is only valid before "
                     "start_all().\n",
                     b->name().c_str());
        assert(false && "TelemetryRegistry::add() called after start_all()");
        return;
    }
    TelemetryBackend* raw = b.get();
    Capabilities caps = raw->capabilities();
    BackendState state{};
    state.backend = raw;
    state.capabilities = caps;
    state.is_source = caps.supported_ops != 0;
    backends_.push_back(std::move(b));
    states_.push_back(std::move(state));
}

void TelemetryRegistry::start_all(const BackendConfig& cfg) {
    std::vector<TelemetryBackend*> processors;
    std::vector<TelemetryBackend*> sources;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_) {
            return;
        }
        started_ = true;
        start_attempted_ = true;
        for (BackendState& state : states_) {
            state.started = false;
            state.status = "not started";
        }
        processors = configured_snapshot_locked(false);
        sources = configured_snapshot_locked(true);
    }

    auto start_backend = [this, &cfg](TelemetryBackend* backend) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (BackendState* state = state_for_locked(backend)) {
                state->started = true;
                state->status = "starting";
            }
        }

        bool ok = backend->start(cfg, [this, backend](TelemetryEvent ev) {
            on_event(backend, std::move(ev));
        });

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (BackendState* state = state_for_locked(backend)) {
                state->started = ok;
                state->status = ok ? "started" : "start failed";
            }
        }
        if (!ok) {
            backend->stop();
        }
    };

    for (TelemetryBackend* backend : processors) start_backend(backend);
    for (TelemetryBackend* backend : sources) start_backend(backend);
}

void TelemetryRegistry::stop_all() {
    std::vector<TelemetryBackend*> sources;
    std::vector<TelemetryBackend*> processors;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) {
            return;
        }
        sources = configured_snapshot_locked(true);
        processors = configured_snapshot_locked(false);
    }

    auto stop_backend = [this](TelemetryBackend* backend) {
        bool should_stop = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (BackendState* state = state_for_locked(backend)) {
                should_stop = state->started;
            }
        }
        if (should_stop) {
            backend->stop();
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (BackendState* state = state_for_locked(backend)) {
                if (should_stop) {
                    state->started = false;
                    state->status = "stopped";
                }
            }
        }
    };

    for (TelemetryBackend* backend : sources) {
        stop_backend(backend);
    }
    for (TelemetryBackend* backend : processors) {
        stop_backend(backend);
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        started_ = false;
    }
}

bool TelemetryRegistry::register_session(const SessionInfo& info) {
    std::vector<TelemetryBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mu_);
        backends = active_backend_snapshot_locked();
    }

    bool ok = true;
    for (TelemetryBackend* backend : backends) {
        ok = backend->register_session(info) && ok;
    }
    return ok;
}

bool TelemetryRegistry::unregister_session(const std::string& cgroup_path) {
    std::vector<TelemetryBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mu_);
        backends = active_backend_snapshot_locked();
    }

    bool ok = true;
    for (TelemetryBackend* backend : backends) {
        ok = backend->unregister_session(cgroup_path) && ok;
    }
    return ok;
}

void TelemetryRegistry::install_policy(const PolicyRules& rules) {
    std::vector<TelemetryBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mu_);
        backends = active_backend_snapshot_locked();
    }

    for (TelemetryBackend* backend : backends) {
        backend->install_policy(rules);
    }
}

Verdict TelemetryRegistry::merge_verdicts(const std::vector<Verdict>& verdicts) {
    Verdict merged = Verdict::Allow;
    for (Verdict verdict : verdicts) {
        merged = std::max(merged, verdict);
    }
    return merged;
}

void TelemetryRegistry::on_event(TelemetryBackend* source, TelemetryEvent event) {
    std::vector<TelemetryBackend*> processors;
    DrainCallback drain;
    {
        std::lock_guard<std::mutex> lock(mu_);
        BackendState* source_state = state_for_locked(source);
        if (!source_state || !source_state->started || !source_state->is_source) {
            return;
        }
        processors = active_processor_snapshot_locked();
        drain = drain_;
    }

    std::vector<Verdict> verdicts;
    verdicts.reserve(processors.size() + 1);
    verdicts.push_back(event.verdict);
    for (TelemetryBackend* processor : processors) {
        // process_event runs on source threads (e.g. FUSE handlers, BPF poll
        // loops). A processor exception leaking out of on_event would cross
        // C-style FUSE callbacks → undefined behavior. Catch all exceptions,
        // log, and skip that processor's contribution. The throwing
        // processor's mutation is dropped (we keep `event` from before the
        // call) but the chain continues.
        TelemetryEvent before = event;
        try {
            event = processor->process_event(event);
            verdicts.push_back(event.verdict);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "telemetry: processor %s threw std::exception: %s — "
                         "skipping its contribution for this event\n",
                         processor->name().c_str(), e.what());
            event = std::move(before);
        } catch (...) {
            std::fprintf(stderr,
                         "telemetry: processor %s threw unknown exception — "
                         "skipping its contribution for this event\n",
                         processor->name().c_str());
            event = std::move(before);
        }
    }
    event.verdict = merge_verdicts(verdicts);

    if (drain) {
        drain(std::move(event));
    }
}

std::vector<TelemetryRegistry::BackendRuntimeStatus>
TelemetryRegistry::backend_statuses() const {
    std::vector<BackendRuntimeStatus> statuses;
    std::lock_guard<std::mutex> lock(mu_);
    statuses.reserve(states_.size());
    for (const BackendState& state : states_) {
        BackendRuntimeStatus status{};
        status.name = state.backend ? state.backend->name() : "";
        status.capabilities = state.capabilities;
        status.started = state.started;
        status.status = state.status;
        statuses.push_back(std::move(status));
    }
    return statuses;
}

std::vector<TelemetryBackend*>
TelemetryRegistry::configured_snapshot_locked(bool source) const {
    std::vector<TelemetryBackend*> backends;
    backends.reserve(states_.size());
    for (const BackendState& state : states_) {
        if (state.is_source == source && state.backend) {
            backends.push_back(state.backend);
        }
    }
    return backends;
}

std::vector<TelemetryBackend*>
TelemetryRegistry::active_backend_snapshot_locked() const {
    std::vector<TelemetryBackend*> backends;
    backends.reserve(states_.size());
    for (const BackendState& state : states_) {
        if ((!start_attempted_ || state.started) && state.backend) {
            backends.push_back(state.backend);
        }
    }
    return backends;
}

std::vector<TelemetryBackend*>
TelemetryRegistry::active_processor_snapshot_locked() const {
    std::vector<TelemetryBackend*> processors;
    processors.reserve(states_.size());
    for (const BackendState& state : states_) {
        if (!state.is_source && state.started && state.backend) {
            processors.push_back(state.backend);
        }
    }
    return processors;
}

TelemetryRegistry::BackendState*
TelemetryRegistry::state_for_locked(TelemetryBackend* backend) {
    auto it = std::find_if(states_.begin(), states_.end(),
                           [backend](const BackendState& state) {
                               return state.backend == backend;
                           });
    return it == states_.end() ? nullptr : &*it;
}

} // namespace cas
