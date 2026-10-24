// Exception dispatch (search phase), unwinding to a target frame, and the
// RaiseException / _CxxThrowException implementations.

#include <csetjmp>
#include <cstring>
#include <deque>
#include <vector>

#include "common/log.h"
#include "hle/seh/seh.h"
#include "loader/modules.h"
#include "loader/pe_image.h"

namespace fl::seh {
namespace {

thread_local std::deque<ActiveCatch>* t_catches = nullptr;
thread_local StateOverride t_override;
thread_local bool t_unwinding = false;

struct Guard {
    jmp_buf jb;
    size_t catch_depth;
};
thread_local std::vector<Guard*>* t_guards = nullptr;

std::deque<ActiveCatch>& catches() {
    if (!t_catches) t_catches = new std::deque<ActiveCatch>();
    return *t_catches;
}

struct Frame {
    uint64_t pc = 0;
    uint64_t base = 0;
    RuntimeFunction* fn = nullptr;
    void* handler = nullptr;
    void* data = nullptr;
    uint64_t establisher = 0;
    Context ctx;               // context *in* this frame (before unwinding it)
    bool has_override = false;
    int override_state = 0;
};

// Move `ctx` up one frame, describing the frame we left in `f`.
// `markers` counts how many fl_call_funclet markers were crossed.
bool step(Context& ctx, int handler_type, Frame& f, size_t& markers) {
    f.has_override = false;
    if (ctx.Rip == reinterpret_cast<uint64_t>(fl_call_funclet_ret)) {
        // Frames between a running catch funclet and its parent are dead:
        // continue in the parent, in the catch's EH state.
        ActiveCatch* ac = active_catch(markers++);
        if (!ac) return false;
        ctx = ac->parent_ctx;
        f.has_override = true;
        f.override_state = ac->entry_state;
    }
    f.pc = ctx.Rip;
    f.ctx = ctx;
    f.fn = lookup_function(f.pc, &f.base);
    if (!f.fn) {
        if (!loader::image_for(reinterpret_cast<void*>(f.pc))) return false;  // left the game's code
        // Leaf function: no prolog, return address on top of the stack.
        f.handler = nullptr;
        f.establisher = ctx.rsp();
        ctx.Rip = *reinterpret_cast<uint64_t*>(ctx.rsp());
        ctx.rsp() += 8;
        return true;
    }
    f.handler = virtual_unwind(handler_type, f.base, f.pc, f.fn, &ctx, &f.data, &f.establisher);
    return true;
}

int call_handler(ExceptionRecord* rec, const Context* orig, Frame& f) {
    DispatcherContext dc{};
    dc.ControlPc = f.pc;
    dc.ImageBase = f.base;
    dc.FunctionEntry = f.fn;
    dc.EstablisherFrame = f.establisher;
    dc.ContextRecord = &f.ctx;
    dc.LanguageHandler = f.handler;
    dc.HandlerData = f.data;
    StateOverride saved = t_override;
    if (f.has_override) t_override = {f.establisher, f.override_state, true};
    Context orig_copy = *orig;  // handlers may scribble on it
    int r = reinterpret_cast<LanguageHandler>(f.handler)(rec, f.establisher, &orig_copy, &dc);
    t_override = saved;
    return r;
}

const char* describe_cxx(const ExceptionRecord* rec) {
    // First catchable type name of a C++ exception, for "unhandled" reports.
    static thread_local char buf[256];
    if (rec->ExceptionCode != kCxxExceptionCode || rec->NumberParameters < 3 || !rec->ExceptionInformation[2])
        return "";
    uint64_t tib = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3]
                                              : reinterpret_cast<uint64_t>(loader::main_image().base);
    auto* ti = reinterpret_cast<const uint32_t*>(rec->ExceptionInformation[2]);
    auto* cta = reinterpret_cast<const int32_t*>(tib + ti[3]);
    if (cta[0] < 1) return "";
    auto* ct = reinterpret_cast<const uint32_t*>(tib + static_cast<uint32_t>(cta[1]));
    snprintf(buf, sizeof buf, " (C++ type %s)", reinterpret_cast<const char*>(tib + ct[1] + 16));
    return buf;
}

}  // namespace

ActiveCatch* active_catch(size_t depth) {
    auto& c = catches();
    return depth < c.size() ? &c[c.size() - 1 - depth] : nullptr;
}
size_t active_catch_count() { return catches().size(); }
void push_active_catch(const ActiveCatch& c) { catches().push_back(c); }
void pop_active_catch() { catches().pop_back(); }
StateOverride& state_override() { return t_override; }
bool& unwinding_in_progress() { return t_unwinding; }

int walk_stack(Context ctx, uint64_t* pcs, int max) {
    int n = 0;
    size_t markers = 0;
    Frame f;
    while (n < max && step(ctx, 0, f, markers)) pcs[n++] = f.pc;
    return n;
}

void dispatch(ExceptionRecord* rec, Context* orig) {
    Context ctx = *orig;
    size_t markers = 0;
    Frame f;
    int depth = 0;
    while (step(ctx, 1 /* UNW_FLAG_EHANDLER */, f, markers)) {
        if (++depth > 100000) break;
        if (!f.handler) continue;
        int r = call_handler(rec, orig, f);
        if (r == ExceptionContinueSearch) continue;
        if (r == ExceptionContinueExecution) {
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
                fl::fatal("handler tried to continue a non-continuable exception 0x%08x", rec->ExceptionCode);
            return;
        }
        fl::fatal("exception handler returned unsupported disposition %d", r);
    }
    if (f.pc == reinterpret_cast<uint64_t>(fl_guarded_call_ret) && t_guards && !t_guards->empty()) {
        // Reached a native catch-all boundary: unwind every game frame up to it
        // (running their unwind handlers), then resume at the boundary.
        FL_LOG(seh, "exception 0x%08x%s caught at a native boundary", rec->ExceptionCode, describe_cxx(rec));
        Context uctx = *orig;
        size_t umarkers = 0;
        Frame uf;
        rec->ExceptionFlags |= EXCEPTION_UNWINDING;
        while (step(uctx, 2 /* UNW_FLAG_UHANDLER */, uf, umarkers))
            if (uf.handler) call_handler(rec, orig, uf);
        Guard* g = t_guards->back();
        while (active_catch_count() > g->catch_depth) pop_active_catch();
        longjmp(g->jb, 1);
    }
    // The game frames the exception passed through, innermost first.
    uint64_t pcs[32];
    int n = walk_stack(*orig, pcs, 32);
    for (int i = 0; i < n; i++) {
        const loader::PeImage* img = loader::image_for(reinterpret_cast<void*>(pcs[i]));
        const char* mod = img ? strrchr(img->path.c_str(), '/') : nullptr;
        FL_ERROR("  #%-2d %s+0x%llx", i, mod ? mod + 1 : "?",
                 static_cast<unsigned long long>(img ? pcs[i] - reinterpret_cast<uint64_t>(img->base) : pcs[i]));
    }
    uint64_t base = reinterpret_cast<uint64_t>(loader::main_image().base);
    fl::fatal("UNHANDLED exception 0x%08x%s raised at game rva 0x%llx (walk stopped at %p)", rec->ExceptionCode,
              describe_cxx(rec), static_cast<unsigned long long>(orig->Rip - base), reinterpret_cast<void*>(f.pc));
}

bool guarded_call(void(WINAPI* fn)(void*), void* arg) {
    if (!t_guards) t_guards = new std::vector<Guard*>();
    Guard g;
    g.catch_depth = active_catch_count();
    t_guards->push_back(&g);
    if (setjmp(g.jb) == 0) {
        fl_guarded_call(reinterpret_cast<void*>(fn), arg);
        t_guards->pop_back();
        return true;
    }
    t_guards->pop_back();
    t_unwinding = false;
    return false;
}

void unwind_to(ExceptionRecord* rec, const Context* throw_ctx, uint64_t target_frame, Context* out) {
    Context ctx = *throw_ctx;
    size_t markers = 0;
    Frame f;
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    while (step(ctx, 2 /* UNW_FLAG_UHANDLER */, f, markers)) {
        bool target = f.establisher == target_frame;
        if (target) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
        if (f.handler) call_handler(rec, throw_ctx, f);
        if (target) {
            rec->ExceptionFlags &= ~(EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND);
            *out = f.ctx;
            // Catch blocks whose frames we unwound through are abandoned.
            for (size_t i = 0; i < markers; i++) pop_active_catch();
            return;
        }
    }
    fl::fatal("unwind: target frame 0x%llx not found", static_cast<unsigned long long>(target_frame));
}

}  // namespace fl::seh

// ---------------------------------------------------------------- entry points (called by the asm stubs)

using namespace fl::seh;

extern "C" void fl_raise_exception_impl(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args, Context* ctx) {
    if (code == 0x406D1388) return;  // MSVC SetThreadName: nobody listens
    ExceptionRecord rec{};
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx->Rip);
    rec.NumberParameters = nargs > 15 ? 15 : nargs;
    for (DWORD i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = args[i];
    FL_LOG(seh, "RaiseException 0x%08x at %p", code, rec.ExceptionAddress);
    dispatch(&rec, ctx);
}

// Defined in cxx_eh.cpp.
void cxx_throw(void* obj, void* throw_info, Context* ctx, bool heap_object);

extern "C" void fl_cxx_throw_impl(void* obj, void* throw_info, uint64_t, uint64_t, Context* ctx) {
    cxx_throw(obj, throw_info, ctx, false);
}
