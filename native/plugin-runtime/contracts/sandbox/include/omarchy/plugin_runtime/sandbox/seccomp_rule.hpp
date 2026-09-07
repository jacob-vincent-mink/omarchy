#pragma once

#include "policy.h"

#include <seccomp.h>
#include <string_view>

namespace omarchy::plugin_runtime::sandbox {

inline int allow_syscall(scmp_filter_ctx context, int number,
                         std::string_view name, const ClonePolicy &clone) {
  if (name == "clone") {
    const auto mask = clone.required_flags | clone.forbidden_flags;
    return seccomp_rule_add(context, SCMP_ACT_ALLOW, number, 1,
                            SCMP_A0(SCMP_CMP_MASKED_EQ, mask,
                                    clone.required_flags));
  }
  return seccomp_rule_add(context, SCMP_ACT_ALLOW, number, 0);
}

} // namespace omarchy::plugin_runtime::sandbox
