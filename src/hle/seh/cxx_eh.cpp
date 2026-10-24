// MSVC C++ exception handling (x64): __CxxFrameHandler3, _CxxThrowException,
// __C_specific_handler, std::exception_ptr support.
//
// EH data structures (all addresses are 32-bit RVAs):
//   FuncInfo   { magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap,
//                nIPMapEntries, pIPToStateMap, dispUnwindHelp, pESTypeList, EHFlags }
//   UnwindMapEntry { toState, action }
//   TryBlockMapEntry { tryLow, tryHigh, catchHigh, nCatches, pHandlerArray }
//   HandlerType { adjectives, pType, dispCatchObj, addressOfHandler, dispFrame }
//   IPToStateMapEntry { ip, state }
//   ThrowInfo { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray }   (RVAs from ExceptionInformation[3])
//   CatchableType { properties, pType, PMD{mdisp,pdisp,vdisp}, sizeOrOffset, copyFunction }
//
// Catch blocks run "deep": the catch funclet is called from here, on top of
// the dead frames, so the thrown object (which may live in a dead frame)
// stays valid during the catch. Afterwards we restore the parent frame's
// context at the funclet's continuation address.

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "hle/seh/seh.h"
#include "loader/modules.h"
#include "loader/pe_image.h"

namespace fl::seh {
namespace {

struct FuncInfo {
    uint32_t magic;
    int32_t max_state;
    uint32_t unwind_map;
    uint32_t n_try;
    uint32_t try_map;
    uint32_t n_ip;
    uint32_t ip_map;
    int32_t unwind_help;
    uint32_t es_types;
    int32_t flags;
};
struct UnwindMapEntry {
    int32_t to_state;
    uint32_t action;
};
struct TryBlock {
    int32_t try_low, try_high, catch_high, n_catches;
    uint32_t handlers;
};
struct HandlerType {
    uint32_t adjectives;
    uint32_t type;
    int32_t disp_catch_obj;
    uint32_t handler;
    uint32_t disp_frame;
};
struct IpToState {
    uint32_t ip;
    int32_t state;
};
struct ThrowInfo {
    uint32_t attributes, unwind_fn, forward_compat, catchable_types;
};
struct CatchableType {
    uint32_t properties, type;
    int32_t mdisp, pdisp, vdisp;
    int32_t size;
    uint32_t copy_fn;
};

constexpr uint32_t CT_IsSimpleType = 1;
constexpr uint32_t CT_ByReferenceOnly = 2;
constexpr uint32_t CT_HasVirtualBase = 4;
constexpr uint32_t HT_IsReference = 8;
constexpr uint32_t kMagic0 = 0x19930520, kMagic2 = 0x19930522;

thread_local int t_target_state = -1;

// ---------------------------------------------------------------- helpers

int ip_to_state(const FuncInfo* fi, uint64_t base, uint32_t ip) {
    auto* map = reinterpret_cast<const IpToState*>(base + fi->ip_map);
    uint32_t i = 0;
    for (; i < fi->n_ip; i++)
        if (map[i].ip > ip) break;
    return i ? map[i - 1].state : -1;
}

void local_unwind(const FuncInfo* fi, uint64_t base, uint64_t frame, int from, int to) {
    auto* map = reinterpret_cast<const UnwindMapEntry*>(base + fi->unwind_map);
    int s = from;
    while (s > to) {
        if (s < 0 || s >= fi->max_state) fl::fatal("C++ EH: bad unwind state %d (max %d)", s, fi->max_state);
        UnwindMapEntry e = map[s];
        s = e.to_state;
        if (e.action) {
            FL_LOG(seh, "  unwind funclet rva 0x%x (state -> %d)", e.action, s);
            fl_call_funclet(reinterpret_cast<void*>(base + e.action), 0, frame);
        }
    }
}

const char* type_name(uint64_t tdesc) { return reinterpret_cast<const char*>(tdesc + 16); }

bool same_type(uint64_t a, uint64_t b) { return a == b || strcmp(type_name(a), type_name(b)) == 0; }

uint8_t* this_adjust(const CatchableType* ct, uint8_t* obj) {
    if (!obj) return obj;
    uint8_t* p = obj + ct->mdisp;
    if (ct->pdisp >= 0) {
        auto* vbtable = *reinterpret_cast<int32_t**>(obj + ct->pdisp);
        p += ct->pdisp + vbtable[ct->vdisp / 4];
    }
    return p;
}

uint64_t throw_image_base(const ExceptionRecord* rec) {
    return rec->NumberParameters >= 4 ? rec->ExceptionInformation[3]
                                      : reinterpret_cast<uint64_t>(loader::main_image().base);
}

// Copy the thrown object into the catch variable.
void build_catch_object(const HandlerType* ht, uint64_t base, uint64_t frame, const CatchableType* ct, uint64_t tib,
                        uint8_t* obj) {
    if (!ht->type || !ht->disp_catch_obj) return;  // catch(...) or unnamed catch
    auto* dest = reinterpret_cast<uint8_t*>(frame + ht->disp_catch_obj);
    if (ht->adjectives & HT_IsReference) {
        *reinterpret_cast<uint8_t**>(dest) = this_adjust(ct, obj);
    } else if (ct->properties & CT_IsSimpleType) {
        memcpy(dest, obj, ct->size);
        if (ct->size == 8) *reinterpret_cast<uint8_t**>(dest) = this_adjust(ct, *reinterpret_cast<uint8_t**>(dest));
    } else if (!ct->copy_fn) {
        memcpy(dest, this_adjust(ct, obj), ct->size);
    } else if (ct->properties & CT_HasVirtualBase) {
        reinterpret_cast<void(WINAPI*)(void*, void*, int)>(tib + ct->copy_fn)(dest, this_adjust(ct, obj), 1);
    } else {
        reinterpret_cast<void(WINAPI*)(void*, void*)>(tib + ct->copy_fn)(dest, this_adjust(ct, obj));
    }
    if (ht->adjectives & 0x10) FL_LOG(seh, "catch of a WinRT handle type (adjective 0x10): no AddRef done");
}

void destroy_thrown(const ExceptionRecord* rec) {
    auto* obj = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
    auto* ti = reinterpret_cast<const ThrowInfo*>(rec->ExceptionInformation[2]);
    if (obj && ti && ti->unwind_fn)
        reinterpret_cast<void(WINAPI*)(void*)>(throw_image_base(rec) + ti->unwind_fn)(obj);
    if (rec->NumberParameters >= 5 && rec->ExceptionInformation[4] == 1) free(obj);  // exception_ptr copy
}

// Found a matching catch: unwind, run the catch funclet, resume after it.
[[noreturn]] void run_catch(ExceptionRecord* rec, const Context* throw_ctx, DispatcherContext* dc, const FuncInfo* fi,
                            uint64_t frame, uint64_t parent_frame, const TryBlock* tb, const HandlerType* ht,
                            const CatchableType* ct) {
    uint64_t base = dc->ImageBase;
    uint64_t tib = throw_image_base(rec);
    auto* obj = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
    FL_LOG(seh, "catching %s in function rva 0x%x (try %d..%d), handler rva 0x%x",
           ct ? type_name(tib + ct->type) : "(...)", dc->FunctionEntry->BeginAddress, tb->try_low, tb->try_high,
           ht->handler);

    if (ct) build_catch_object(ht, base, parent_frame, ct, tib, obj);

    // Unwind everything above this frame, and this frame down to the try level.
    t_target_state = tb->try_low;
    ExceptionRecord rec_copy = *rec;
    Context parent;
    unwind_to(&rec_copy, throw_ctx, frame, &parent);

    ActiveCatch ac;
    ac.parent_ctx = parent;
    ac.frame = parent_frame;
    uint64_t dummy;
    RuntimeFunction* hf = lookup_function(base + ht->handler, &dummy);
    ac.funclet_rva = hf ? hf->BeginAddress : ht->handler;
    ac.entry_state = ip_to_state(fi, base, ht->handler);
    ac.rec = *rec;
    push_active_catch(ac);
    unwinding_in_progress() = false;

    uint64_t cont = fl_call_funclet(reinterpret_cast<void*>(base + ht->handler), 0, parent_frame);

    ActiveCatch done = *active_catch(0);
    pop_active_catch();
    if (!done.rethrown) destroy_thrown(&done.rec);
    FL_LOG(seh, "catch block done, continuing at rva 0x%llx", static_cast<unsigned long long>(cont - base));
    Context resume = done.parent_ctx;
    resume.Rip = cont;
    fl_restore_context(&resume);
}

// ---------------------------------------------------------------- __CxxFrameHandler3

WINAPI int cxx_frame_handler3(ExceptionRecord* rec, uint64_t frame, Context* ctx, DispatcherContext* dc) {
    uint64_t base = dc->ImageBase;
    auto* fi = reinterpret_cast<const FuncInfo*>(base + *static_cast<uint32_t*>(dc->HandlerData));
    if (fi->magic < kMagic0 || fi->magic > kMagic2) {
        FL_ERROR("__CxxFrameHandler3: bad FuncInfo magic 0x%x", fi->magic);
        return ExceptionContinueSearch;
    }

    // Funclets (catch blocks) use their parent function's frame.
    uint64_t parent_frame = frame;
    int funclet_entry_state = -2;
    for (size_t i = 0; i < active_catch_count(); i++) {
        ActiveCatch* ac = active_catch(i);
        if (ac->funclet_rva == dc->FunctionEntry->BeginAddress) {
            parent_frame = ac->frame;
            funclet_entry_state = ac->entry_state;
            break;
        }
    }

    StateOverride& ov = state_override();
    int state = (ov.active && ov.frame == frame) ? ov.state
                                                 : ip_to_state(fi, base, static_cast<uint32_t>(dc->ControlPc - base));

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        int to = (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) ? t_target_state
               : funclet_entry_state != -2                     ? funclet_entry_state
                                                               : -1;
        local_unwind(fi, base, parent_frame, state, to);
        return ExceptionContinueSearch;
    }

    if (rec->ExceptionCode != kCxxExceptionCode || rec->NumberParameters < 3) return ExceptionContinueSearch;
    uint32_t magic = static_cast<uint32_t>(rec->ExceptionInformation[0]);
    if (magic < kMagic0 || magic > kMagic2) return ExceptionContinueSearch;

    uint64_t tib = throw_image_base(rec);
    auto* ti = reinterpret_cast<const ThrowInfo*>(rec->ExceptionInformation[2]);
    auto* cta = reinterpret_cast<const int32_t*>(tib + ti->catchable_types);
    auto* tries = reinterpret_cast<const TryBlock*>(base + fi->try_map);

    for (uint32_t i = 0; i < fi->n_try; i++) {
        const TryBlock* tb = &tries[i];
        if (state < tb->try_low || state > tb->try_high) continue;
        auto* hts = reinterpret_cast<const HandlerType*>(base + tb->handlers);
        for (int j = 0; j < tb->n_catches; j++) {
            const HandlerType* ht = &hts[j];
            if (!ht->type) run_catch(rec, ctx, dc, fi, frame, parent_frame, tb, ht, nullptr);  // catch(...)
            for (int k = 0; k < cta[0]; k++) {
                auto* ct = reinterpret_cast<const CatchableType*>(tib + static_cast<uint32_t>(cta[1 + k]));
                if (!same_type(base + ht->type, tib + ct->type)) continue;
                if ((ct->properties & CT_ByReferenceOnly) && !(ht->adjectives & HT_IsReference)) continue;
                run_catch(rec, ctx, dc, fi, frame, parent_frame, tb, ht, ct);
            }
        }
    }
    return ExceptionContinueSearch;
}

// ---------------------------------------------------------------- __C_specific_handler (__try/__except/__finally)

struct ScopeEntry {
    uint32_t begin, end, handler, target;
};

thread_local uint64_t t_seh_target_ip = 0;

WINAPI int c_specific_handler(ExceptionRecord* rec, uint64_t frame, Context* ctx, DispatcherContext* dc) {
    uint64_t base = dc->ImageBase;
    auto* count = static_cast<uint32_t*>(dc->HandlerData);
    auto* scopes = reinterpret_cast<ScopeEntry*>(count + 1);
    uint32_t pc = static_cast<uint32_t>(dc->ControlPc - base);

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        for (uint32_t i = dc->ScopeIndex; i < *count; i++) {
            ScopeEntry& s = scopes[i];
            if (pc < s.begin || pc >= s.end) continue;
            if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && base + s.target == t_seh_target_ip) break;
            if (s.target == 0) {  // __finally
                dc->ScopeIndex = i + 1;
                FL_LOG(seh, "__finally handler rva 0x%x", s.handler);
                fl_call_funclet(reinterpret_cast<void*>(base + s.handler), 1 /* abnormal */, frame);
            }
        }
        return ExceptionContinueSearch;
    }

    for (uint32_t i = dc->ScopeIndex; i < *count; i++) {
        ScopeEntry& s = scopes[i];
        if (pc < s.begin || pc >= s.end || s.target == 0) continue;
        int32_t verdict = 1;  // EXCEPTION_EXECUTE_HANDLER
        if (s.handler != 1) {
            ExceptionPointers ptrs{rec, ctx};
            verdict = static_cast<int32_t>(
                fl_call_funclet(reinterpret_cast<void*>(base + s.handler), reinterpret_cast<uint64_t>(&ptrs), frame));
        }
        if (verdict < 0) return ExceptionContinueExecution;
        if (verdict == 0) continue;
        FL_LOG(seh, "__except: exception 0x%08x handled at rva 0x%x", rec->ExceptionCode, s.target);
        t_seh_target_ip = base + s.target;
        Context parent;
        unwind_to(rec, ctx, frame, &parent);
        parent.Rip = base + s.target;
        parent.rax() = rec->ExceptionCode;
        unwinding_in_progress() = false;
        fl_restore_context(&parent);
    }
    return ExceptionContinueSearch;
}

// ---------------------------------------------------------------- exception_ptr
// std::exception_ptr is two pointers; we keep a refcounted copy in the first.

struct ExPtr {
    std::atomic<int> refs{1};
    ExceptionRecord rec;  // ExceptionInformation[1] points at `obj`
    void* obj = nullptr;
    size_t size = 0;
};

void* copy_thrown_object(const ExceptionRecord* rec, size_t* size_out) {
    uint64_t tib = throw_image_base(rec);
    auto* ti = reinterpret_cast<const ThrowInfo*>(rec->ExceptionInformation[2]);
    auto* cta = reinterpret_cast<const int32_t*>(tib + ti->catchable_types);
    auto* ct = reinterpret_cast<const CatchableType*>(tib + static_cast<uint32_t>(cta[1]));  // most derived
    auto* src = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
    void* dst = calloc(1, ct->size ? ct->size : 1);
    if ((ct->properties & CT_IsSimpleType) || !ct->copy_fn)
        memcpy(dst, src, ct->size);
    else if (ct->properties & CT_HasVirtualBase)
        reinterpret_cast<void(WINAPI*)(void*, void*, int)>(tib + ct->copy_fn)(dst, src, 1);
    else
        reinterpret_cast<void(WINAPI*)(void*, void*)>(tib + ct->copy_fn)(dst, src);
    *size_out = ct->size;
    return dst;
}

void exptr_release(ExPtr* p) {
    if (p && --p->refs == 0) {
        destroy_thrown(&p->rec);  // runs the destructor and frees (ExceptionInformation[4] == 1)
        delete p;
    }
}

WINAPI void ep_create(void** ep) { ep[0] = ep[1] = nullptr; }
WINAPI void ep_destroy(void** ep) { exptr_release(static_cast<ExPtr*>(ep[0])); }
WINAPI void ep_copy(void** dst, void* const* src) {
    auto* p = static_cast<ExPtr*>(src[0]);
    if (p) ++p->refs;
    dst[0] = dst[1] = p;
}

WINAPI void ep_current(void** ep) {
    ActiveCatch* ac = active_catch(0);
    if (!ac || ac->rec.ExceptionCode != kCxxExceptionCode) {
        if (ac) FL_ERROR("exception_ptr of a non-C++ exception 0x%08x: returning null", ac->rec.ExceptionCode);
        ep[0] = ep[1] = nullptr;
        return;
    }
    auto* p = new ExPtr();
    p->rec = ac->rec;
    p->obj = copy_thrown_object(&ac->rec, &p->size);
    p->rec.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(p->obj);
    p->rec.NumberParameters = 5;
    p->rec.ExceptionInformation[4] = 1;
    ep[0] = ep[1] = p;
}

}  // namespace
}  // namespace fl::seh

using namespace fl::seh;

void cxx_throw(void* obj, void* throw_info, Context* ctx, bool heap_object) {
    ExceptionRecord rec{};
    if (!obj && !throw_info) {
        // `throw;` inside a catch block.
        ActiveCatch* ac = active_catch(0);
        if (!ac) fl::fatal("C++ rethrow with no active exception (would call terminate)");
        ac->rethrown = true;
        rec = ac->rec;
        rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        FL_LOG(seh, "rethrow at game rva 0x%llx",
               static_cast<unsigned long long>(ctx->Rip - reinterpret_cast<uint64_t>(fl::loader::main_image().base)));
    } else {
        rec.ExceptionCode = kCxxExceptionCode;
        rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        rec.NumberParameters = heap_object ? 5 : 4;
        rec.ExceptionInformation[0] = 0x19930520;
        rec.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(obj);
        rec.ExceptionInformation[2] = reinterpret_cast<ULONG_PTR>(throw_info);
        // ThrowInfo RVAs are relative to the image that holds it (the exe or a package DLL).
        fl::loader::PeImage* ti_img = fl::loader::image_for(throw_info);
        rec.ExceptionInformation[3] =
            reinterpret_cast<ULONG_PTR>(ti_img ? ti_img->base : fl::loader::main_image().base);
        rec.ExceptionInformation[4] = heap_object ? 1 : 0;
        FL_LOG(seh, "C++ throw at game rva 0x%llx",
               static_cast<unsigned long long>(ctx->Rip - reinterpret_cast<uint64_t>(fl::loader::main_image().base)));
    }
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx->Rip);
    unwinding_in_progress() = true;
    dispatch(&rec, ctx);
}

extern "C" void fl_exptr_rethrow_impl(void* const* ep, uint64_t, uint64_t, uint64_t, Context* ctx) {
    auto* p = static_cast<ExPtr*>(ep[0]);
    if (!p) fl::fatal("rethrow_exception(null exception_ptr) (would throw bad_exception)");
    size_t size;
    void* copy = copy_thrown_object(&p->rec, &size);
    cxx_throw(copy, reinterpret_cast<void*>(p->rec.ExceptionInformation[2]), ctx, true);
}

namespace fl::seh {
namespace {
WINAPI bool uncaught_exception() { return unwinding_in_progress(); }
}  // namespace
}  // namespace fl::seh

extern "C" void fl_RaiseException_entry();
extern "C" void fl_CxxThrowException_entry();
extern "C" void fl_ExceptionPtrRethrow_entry();

HLE_EXPORT(kernelx, RaiseException, fl_RaiseException_entry);
HLE_EXPORT(msvcr110, _CxxThrowException, fl_CxxThrowException_entry);
HLE_EXPORT(msvcr110, __CxxFrameHandler3, fl::seh::cxx_frame_handler3);
HLE_EXPORT(msvcr110, __C_specific_handler, fl::seh::c_specific_handler);
HLE_EXPORT_AS(msvcr110, "?__ExceptionPtrCreate@@YAXPEAX@Z", fl::seh::ep_create);
HLE_EXPORT_AS(msvcr110, "?__ExceptionPtrDestroy@@YAXPEAX@Z", fl::seh::ep_destroy);
HLE_EXPORT_AS(msvcr110, "?__ExceptionPtrCopy@@YAXPEAXPEBX@Z", fl::seh::ep_copy);
HLE_EXPORT_AS(msvcr110, "?__ExceptionPtrCurrentException@@YAXPEAX@Z", fl::seh::ep_current);
HLE_EXPORT_AS(msvcr110, "?__ExceptionPtrRethrow@@YAXPEBX@Z", fl_ExceptionPtrRethrow_entry);
HLE_EXPORT_AS(msvcp110, "?uncaught_exception@std@@YA_NXZ", fl::seh::uncaught_exception);
