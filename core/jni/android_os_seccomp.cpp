/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "JNIHelp.h"
#include "core_jni_helpers.h"
#include "JniConstants.h"
#include "utils/Log.h"
#include "utils/misc.h"

#if defined __arm__ || defined __aarch64__

#include <vector>

#include <sys/prctl.h>

#include <linux/unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "seccomp_policy.h"

#define syscall_nr (offsetof(struct seccomp_data, nr))
#define arch_nr (offsetof(struct seccomp_data, arch))

typedef std::vector<sock_filter> filter;

// We want to keep the below inline functions for debugging and future
// development even though they are not all sed currently.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static inline void Kill(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL));
}

static inline void Trap(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP));
}

static inline void Error(filter& f, __u16 retcode) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO + retcode));
}

inline static void Trace(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRACE));
}

inline static void Allow(filter& f) {
    f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
}

#pragma clang diagnostic pop

inline static void AllowSyscall(filter& f, __u32 num) {
    f.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, num, 0, 1));
    Allow(f);
}

inline static void ExamineSyscall(filter& f) {
    f.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, syscall_nr));
}

inline static int SetValidateArchitectureJumpTarget(size_t offset, filter& f) {
    size_t jump_length = f.size() - offset - 1;
    auto u8_jump_length = (__u8) jump_length;
    if (u8_jump_length != jump_length) {
        ALOGE("Can't set jump greater than 255 - actual jump is %zu",
              jump_length);
        return -1;
    }
    f[offset] = BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_ARM, u8_jump_length, 0);
    return 0;
}

inline static size_t ValidateArchitectureAndJumpIfNeeded(filter& f) {
    f.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, arch_nr));

    f.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_AARCH64, 2, 0));
    f.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_ARM, 1, 0));
    Trap(f);
    return f.size() - 2;
}

static bool install_filter(filter const& f) {
    struct sock_fprog prog = {
        (unsigned short) f.size(),
        (struct sock_filter*) &f[0],
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        ALOGE("SECCOMP: Could not set seccomp filter of size %zu: %s", f.size(), strerror(errno));
        return false;
    }

    ALOGI("SECCOMP: Global filter of size %zu installed", f.size());
    return true;
}

bool set_seccomp_filter() {
    filter f;

    // Note that for mixed 64/32 bit architectures, ValidateArchitecture inserts a
    // jump that must be changed to point to the start of the 32-bit policy
    // 32 bit syscalls will not hit the policy between here and the call to SetJump
    auto offset_to_32bit_filter =
        ValidateArchitectureAndJumpIfNeeded(f);

    // 64-bit filter
    ExamineSyscall(f);

    // arm64-only filter - autogenerated from bionic syscall usage
    for (size_t i = 0; i < arm64_filter_size; ++i)
        f.push_back(arm64_filter[i]);

    // Syscalls needed to boot Android
    AllowSyscall(f, 41);  // __NR_pivot_root
    AllowSyscall(f, 31);  // __NR_ioprio_get
    AllowSyscall(f, 30);  // __NR_ioprio_set
    AllowSyscall(f, 178); // __NR_gettid
    AllowSyscall(f, 98);  // __NR_futex
    AllowSyscall(f, 220); // __NR_clone
    AllowSyscall(f, 139); // __NR_rt_sigreturn
    AllowSyscall(f, 240); // __NR_rt_tgsigqueueinfo
    AllowSyscall(f, 128); // __NR_restart_syscall
    AllowSyscall(f, 278); // __NR_getrandom

    // Needed for performance tools
    AllowSyscall(f, 241); // __NR_perf_event_open

    // Needed for strace
    AllowSyscall(f, 130); // __NR_tkill

    // Needed for kernel to restart syscalls
    AllowSyscall(f, 128); // __NR_restart_syscall

    // b/35034743
    AllowSyscall(f, 267); // __NR_fstatfs64

    Trap(f);

    if (SetValidateArchitectureJumpTarget(offset_to_32bit_filter, f) != 0)
        return -1;

    // 32-bit filter
    ExamineSyscall(f);

    // arm32 filter - autogenerated from bionic syscall usage
    for (size_t i = 0; i < arm_filter_size; ++i)
        f.push_back(arm_filter[i]);

    // Syscalls needed to boot android
    AllowSyscall(f, 120); // __NR_clone
    AllowSyscall(f, 240); // __NR_futex
    AllowSyscall(f, 119); // __NR_sigreturn
    AllowSyscall(f, 173); // __NR_rt_sigreturn
    AllowSyscall(f, 363); // __NR_rt_tgsigqueueinfo
    AllowSyscall(f, 224); // __NR_gettid

    // Syscalls needed to run Chrome
    AllowSyscall(f, 383); // __NR_seccomp - needed to start Chrome
    AllowSyscall(f, 384); // __NR_getrandom - needed to start Chrome

    // Syscalls needed to run GFXBenchmark
    AllowSyscall(f, 190); // __NR_vfork

    // Needed for strace
    AllowSyscall(f, 238); // __NR_tkill

    // Needed for kernel to restart syscalls
    AllowSyscall(f, 0);   // __NR_restart_syscall

    // Needed for debugging 32-bit Chrome
    AllowSyscall(f, 42);  // __NR_pipe

    // b/34732712
    AllowSyscall(f, 364); // __NR_perf_event_open

    // b/34651972
    AllowSyscall(f, 33);  // __NR_access
    AllowSyscall(f, 195); // __NR_stat64

    // b/34813887
    AllowSyscall(f, 5);   // __NR_open
    AllowSyscall(f, 141); // __NR_getdents
    AllowSyscall(f, 217); // __NR_getdents64

    // b/34719286
    AllowSyscall(f, 351); // __NR_eventfd

    // b/34817266
    AllowSyscall(f, 252); // __NR_epoll_wait

    // Needed by sanitizers (b/34606909)
    // 5 (__NR_open) and 195 (__NR_stat64) are also required, but they are
    // already allowed.
    AllowSyscall(f, 85);  // __NR_readlink

    // b/34908783
    AllowSyscall(f, 250); // __NR_epoll_create

    // b/34979910
    AllowSyscall(f, 8);   // __NR_creat
    AllowSyscall(f, 10);  // __NR_unlink

    // b/35059702
    AllowSyscall(f, 196); // __NR_lstat64

    Trap(f);

    return install_filter(f);
}

static void Seccomp_setPolicy(JNIEnv* /*env*/) {
    if (!set_seccomp_filter()) {
        ALOGE("Failed to set seccomp policy - killing");
        exit(1);
    }
}

#else // #if defined __arm__ || defined __aarch64__

static void Seccomp_setPolicy(JNIEnv* /*env*/) {
}

#endif

static const JNINativeMethod method_table[] = {
    NATIVE_METHOD(Seccomp, setPolicy, "()V"),
};

namespace android {

int register_android_os_seccomp(JNIEnv* env) {
    return android::RegisterMethodsOrDie(env, "android/os/Seccomp",
                                         method_table, NELEM(method_table));
}

}
