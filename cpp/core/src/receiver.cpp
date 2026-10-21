#include "codenet/receiver.hpp"

namespace codenet {

const char* roleName(Role role) { return role == Role::Main ? "main" : "backup"; }

std::optional<ScheduledFrame> Schedule::frameAt(int64_t localNs) const {
    if (!active) return std::nullopt;
    const int rate = anchor.rate;
    const int64_t start = anchorStartNs - outputAdvanceNs;
    const int64_t elapsed = localNs - start;
    if (elapsed < 0) return std::nullopt;
    const int64_t k = elapsed * rate / kNsPerSecond;
    if (k > maxPredictFrames) return std::nullopt;
    return ScheduledFrame{anchor.next(uint32_t(k)), userBits, start + frameOffsetNs(k, rate)};
}

Receiver::Receiver(ReceiverConfig config) : config_(config) {
    setSources(config.main, config.backup, 0);
    switches_.clear();
    returnRequested_ = false;
}

SourceState* Receiver::find(const SourceId& source) {
    for (auto& s : sources_)
        if (s.source == source) return &s;
    return nullptr;
}

const SourceState* Receiver::active() const {
    if (!active_) return nullptr;
    for (const auto& s : sources_)
        if (s.source == *active_) return &s;
    return nullptr;
}

void Receiver::onFrame(const FramePacket& packet, int64_t nowNs) {
    SourceState* state = find(packet.source);
    if (state == nullptr) return;  // not one of our sources
    state->hasPacket = true;
    state->lastPacket = packet;
    state->lastRxNs = nowNs;
    if ((packet.flags & kFlagPlaying) && state->clock.ready()) {
        state->hasAnchor = true;
        state->anchor = packet.timecode;
        state->anchorStartNs = state->clock.hostToLocal(packet.frameStartNs);
    } else if (!(packet.flags & kFlagPlaying)) {
        state->hasAnchor = false;
    }
    update(nowNs);
}

void Receiver::onSync(const SourceId& source, int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
    if (SourceState* state = find(source)) state->clock.add(t1, t2, t3, t4);
}

void Receiver::setSources(std::optional<SourceId> main, std::optional<SourceId> backup, int64_t nowNs) {
    if (main && isEmpty(*main)) main.reset();
    if (backup && isEmpty(*backup)) backup.reset();
    std::vector<SourceState> old;
    old.swap(sources_);
    config_.main = main;
    config_.backup = backup;
    for (auto [source, role] : {std::pair{main, Role::Main}, std::pair{backup, Role::Backup}}) {
        if (!source) continue;
        SourceState state;
        for (auto& o : old)
            if (o.source == *source) state = o;
        state.source = *source;
        state.role = role;
        sources_.push_back(state);
    }
    if (active_ && find(*active_) == nullptr) {
        std::optional<Role> was;
        for (auto& o : old)
            if (o.source == *active_) was = o.role;
        switches_.push_back({nowNs, was, std::nullopt, "host unassigned"});
        active_.reset();
    }
    returnRequested_ = true;  // follow the newly chosen main straight away if it is valid
    update(nowNs);
}

bool Receiver::isValid(const SourceState& state, int64_t nowNs) const {
    // A stopped transport is still valid: it is a deliberate stop, not a loss.
    if (!state.hasPacket || !state.clock.ready()) return false;
    if (state.lastPacket.flags & kFlagSourceLost) return false;
    const int64_t timeout = frameOffsetNs(config_.lossTimeoutFrames, state.lastPacket.timecode.rate);
    return nowNs - state.lastRxNs < timeout;
}

void Receiver::update(int64_t nowNs) {
    SourceState* main = config_.main ? find(*config_.main) : nullptr;
    SourceState* backup = config_.backup ? find(*config_.backup) : nullptr;
    for (auto& s : sources_) {
        if (isValid(s, nowNs)) {
            if (!s.validSinceNs) s.validSinceNs = nowNs;
        } else {
            s.validSinceNs.reset();
        }
    }
    const bool mainOk = main != nullptr && main->validSinceNs.has_value();
    const bool backupOk = backup != nullptr && backup->validSinceNs.has_value();
    SourceState* current = active_ ? find(*active_) : nullptr;

    SourceState* target = nullptr;
    std::string reason;
    if (current != nullptr && current == main && mainOk) {
        target = main;
    } else if (current != nullptr && current == backup && backupOk) {
        target = backup;
        const bool stable = mainOk && nowNs - *main->validSinceNs >= config_.returnToMainAfterNs;
        if (mainOk && (returnRequested_ || (config_.autoReturn && stable))) {
            target = main;
            reason = "returned to main";
        }
    } else if (mainOk) {
        target = main;
        reason = "main valid";
    } else if (backupOk) {
        target = backup;
        reason = current == main && current != nullptr ? "main lost" : "backup valid";
    } else {
        reason = "no valid source";
    }

    if (target != current) {
        switches_.push_back({nowNs, current ? std::optional<Role>(current->role) : std::nullopt,
                             target ? std::optional<Role>(target->role) : std::nullopt, reason});
        if (switches_.size() > 64) switches_.erase(switches_.begin());
        active_ = target ? std::optional<SourceId>(target->source) : std::nullopt;
    }
    if (target != nullptr && target == main) returnRequested_ = false;
}

Schedule Receiver::schedule() const {
    Schedule s;
    s.maxPredictFrames = config_.maxPredictFrames;
    s.outputAdvanceNs = config_.outputAdvanceNs;
    if (const SourceState* a = active(); a != nullptr && a->hasAnchor) {
        s.active = true;
        s.anchor = a->anchor;
        s.anchorStartNs = a->anchorStartNs;
        s.userBits = a->hasPacket ? a->lastPacket.userBits : 0;
    }
    return s;
}

}  // namespace codenet
