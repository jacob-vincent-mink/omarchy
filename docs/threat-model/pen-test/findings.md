# Pen-test findings

Every pen-test case, the boundary it attacks, the control that contains it, and
the result. White-box cases are in `native/ward/tests/pen_test.rs`; black-box
cases are in `native/ward/tests/pen_test_blackbox.rs`. This is the original case ledger, not evidence of exhaustive coverage or a green integration suite. The [September 10 review corrections](../review-regressions.md) supersede the earlier security conclusions.

| # | Pass | Case | Boundary | Attack | Control | Result |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | white | exec traversal/overflow | TB-8/TB-4 | `$OMARCHY_PLUGIN_PATH/../etc/passwd` and resolved-form traversal | token resolves only within the pinned dir; `..`/`.` guarded | Contained |
| 2 | white | exec extra/null/oversized | TB-8 | a second argument, empty, null byte, over-bound | a complete argv path is required | Contained |
| 3 | white | exec integer/pattern | TB-8 | `01`/`+1`/`1e1`; prefix/traversal/encoded/newline | whole-argument, bounded match | Contained |
| 4 | white | exec selection | TB-8 | a non-leaf or parent name selected | selected names must come from the tree | Contained |
| 5 | white | http scope def | TB-8 | bad `*`, subtree, non-canonical origin, credentials, fragment, body on GET, unknown field | `Scope::validate` is strict | Contained |
| 6 | white | grants | TB-2 | a missing required scope; an unrequested grant | a gap is a gap; unrequested fails | Contained |
| 7 | white | fs access/target | TB-2 | read vs read-write; write-only selection | read ≠ write; write-only is `Unsupported` | Contained |
| 8 | white | settings keys | TB-8 | `*`/`id`/`sandbox`/`constructor`/`__proto__`/`prototype` | host-structure and wildcard keys rejected | Contained |
| 9 | white | settings write | TB-8 | a write to a read-only key; adding a host-structure key | writes bounded to approved keys | Contained |
| 10 | white | notification text | TB-8 | control/RTL chars, empty/oversized | text policy; body escaped; title prefixed | Contained |
| 11 | white | notification packet | TB-8 | extra/`exec`/duplicate field, wrong version, carried fd, oversized | `Request::decode` is strict | Contained |
| 12 | white | context | TB-6 | oversized, non-finite/zero bar, bad position, panel serial 0 / payload-when-closed, unknown field | bounded, finite, strict | Contained |
| 13 | white | presentation record | TB-6 | generation 0, bad dimensions/scale, missing magic, bad slot/serial, mask over cap | strict slot/generation | Contained |
| 14 | white | presentation buffer | TB-6 | over pixel cap, odd/short stride, missing fd | dimension + stride bounds | Contained |
| 15 | black | self-approve file | TB-1/TB-2 | manifest requests `/etc/passwd` | grant construction/schema validation only; does not exercise store approval or a real worker | Validator only |
| 16 | black | exec arg smuggle | TB-8 | a second argument to a reviewed command | extra argument rejected (free-text slot is by design) | Contained* |
| 17 | black | http origin | TB-8 | malformed scope definitions | `Scope::validate` only; request matching has separate module tests | Validator only |
| 18 | black | settings write | TB-8 | write a read-only or host-structure key | writes bounded to approved keys | Contained |
| 19 | black | notification markup | TB-8 | markup, a command-looking title, control/RTL | escaped + prefixed + text policy | Contained |
| 20 | black | context | TB-6 | oversized / unknown field / non-finite coordinate | the plugin only reads the host-published context | Contained |
| 21 | black | asset traversal | TB-4/TB-8 | `$OMARCHY_PLUGIN_PATH/..` out of the assets | the token resolves only within the pinned dir | Contained |

\* Case 16 is the one **Medium, by-design** residual: a free-text exec argument
accepts any single string, so argv matching is not semantic safety. The reviewer's
selection and revocation are the boundary, not the matcher
([`../04-stride-and-trust.md`](../04-stride-and-trust.md), TB-8).

## Summary

Security-critical fixes and additional end-to-end tests are required by the subsequent PR review. Retain these narrow guards, but do not infer that untested attacks are contained. Cases 13/14 include sender-side validation as well as a few decoder checks; adversarial receiver bytes need their own evidence. Approved CLI semantics, graphics driver/parser exposure and same-account trust remain distinct residuals, not a single medium-risk exception.
