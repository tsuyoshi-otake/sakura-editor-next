# JSONC Configuration Editing Tests

- Keep tests fake-service based.  They must not access a local settings file or
  start the editor process.
- Cover byte preservation and terminal filesystem/CAS outcomes independently;
  diagnostics must remain free of resource identities, keys, and values.
