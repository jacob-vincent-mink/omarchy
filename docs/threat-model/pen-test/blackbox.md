# Black-box pen-test

The black-box pass works from only the plugin authoring docs, as a plugin
author would. It does not read the Rust source; it tries the obvious things the
docs imply and is surprised (or not) by the host's response. Each case is in
`native/ward/tests/pen_test_blackbox.rs`. All 7 cases pass.

## What a docs-only author would try

The authoring docs tell a plugin it can request host files, call host commands,
make HTTP requests, write settings, notify, and get UI context. A docs-only
author, without the source, would try to get more than they were promised.

### "I can request host files" → I request `/etc/passwd`

`blackbox_a_plugin_cannot_self_approve_a_host_file_grant`. The manifest only
*requests*; it approves nothing. A grant the reviewer never made cannot validate
against the request, and granting something the plugin never requested is
rejected. The reviewer's chosen directory — never `/etc/passwd` — is what the
grant points at. **Contained by TB-1/TB-2.**

### "I can call host commands" → I smuggle a flag

`blackbox_a_plugin_cannot_add_arguments_to_a_reviewed_command`. The plugin is
granted `omarchy-pkg-add <name>` (one free-text argument). A second argument is
rejected. A single free-text argument is accepted **by design** — the reviewer
chose a free-text slot; the protection is against *extra* arguments, not the
content of the reviewed slot. **Contained by TB-8 (with the documented semantic
limitation).**

### "I can make HTTP requests" → I hit another origin

`blackbox_a_plugin_cannot_request_an_unapproved_http_origin`. The plugin is
granted `api.example.test`. It tries `api.example.test.evil.test`, a different
port, scheme, credentials, path, and query. The granted scope is a valid, exact
selection; an unrecognized field is a schema error, not a widening; a `*` is one
segment only. The request-vs-scope match (the private `check`) rejects the
mismatch, as its module test proves. **Contained by TB-8.**

### "I can write settings" → I write a read-only or host-structure key

`blackbox_a_plugin_cannot_write_a_read_only_or_host_structure_key`. The reviewer
granted read of `theme` and write of `volume`. Writing `volume` is allowed;
writing `theme` (read-only) is rejected; widening the write set to
`constructor` is rejected at validation. **Contained by TB-8.**

### "I can notify" → I smuggle markup or a command

`blackbox_a_plugin_cannot_smuggle_markup_into_a_notification`. Markup is
escaped; a null byte or newline in the title is rejected; a RTL override
character is rejected; the title is prefixed so it cannot be re-parsed as a CLI
option. **Contained by TB-8.**

### "I get UI context" → I read a malformed or oversized context

`blackbox_a_plugin_cannot_get_a_malformed_or_oversized_context`. The plugin
only reads the host-published context file; it cannot construct one. An
oversized context, an unknown field, and a non-finite bar coordinate are all
rejected. **Contained by TB-6.**

### "I can read my own assets" → I traverse out of them

`blackbox_a_plugin_cannot_traverse_out_of_its_assets`. The plugin uses the
plugin-path token. A legitimate asset is accepted; the obvious traversal
attempts (`../`, `../../`, `./`, trailing `/`) are all rejected. **Contained by
TB-4/TB-8.**

## Conclusion

A docs-only author, trying the obvious things the docs imply, is contained at
every boundary. The manifest requests; the host decides. A request reaches the
host only through an authenticated, rate-limited, re-checked broker; a granted
capability is matched exactly, never widened. The only semantic gap — a free-text
exec argument — is the same intentional TB-8 limitation the white-box pass
found, and it is bounded by the reviewer's selection and revocation.
