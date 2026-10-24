# Security policy

SANAD Engine is a native Windows x64 game engine and editor written in C++23
with a Vulkan renderer. This policy covers the engine, the editor, the `nf` CLI,
and the packaged player.

## Supported versions

| Version | Supported |
|:---|:---:|
| `main` (latest commit) | ✅ |
| 0.5.0 public release | ✅ (security fixes only) |
| 0.4.0 and older | ❌ |

Security fixes land on `main` first. There is no long-term support branch yet.

## Report a vulnerability

**Do not open a public issue for a working exploit.**

Use GitHub's private vulnerability reporting:

1. Open <https://github.com/abdallah2183/SANAD-Engine/security/advisories/new>
2. Choose **Report a vulnerability**
3. Include affected version/commit, platform, reproduction steps, impact, and a
   minimal proof of concept if one exists.

If private reporting is unavailable, contact the maintainer through the GitHub
profile at <https://github.com/abdallah2183> and ask for a private channel before
sharing details.

You can expect an acknowledgement within **7 days** and a status update within
**30 days**. Please allow time for a fix before publishing.

## Scope

In scope:

- Memory-safety bugs in the engine, editor, CLI, asset importers, or player.
- Sandbox escapes in Lua or the .NET script host.
- Path traversal or arbitrary file write through project/asset handling.
- Malicious `.nfscene`, `.nfmesh`, `.nfmat`, glTF, or image files that crash or
  execute code.
- Privilege boundaries broken by the editor or packaging scripts.

Out of scope:

- Crashes caused by unsupported GPUs, drivers, or missing Vulkan support.
- Smart App Control / WDAC blocking the self-signed **`SANAD Dev Build`**
  binaries. That is expected until a publicly trusted code-signing certificate
  is used; do not disable your security policy to work around it.
- Denial of service from deliberately enormous but otherwise valid projects.
- Issues in third-party dependencies that are already fixed upstream and not
  reachable through SANAD.

## Binary signing

The 0.5.0 release is a **portable zip** signed with the self-signed
`SANAD Dev Build` certificate, not a publicly trusted code-signing certificate.
Windows may show "Unknown publisher" or block a downloaded executable. Verify the
SHA-256 from the release page and build from source if you need a binary you
trust. A publicly trusted code-signing certificate is on the roadmap; it is not a
substitute for reporting a vulnerability.

## Disclosure

We credit reporters in the advisory unless you ask to remain anonymous. Please
do not include secrets, personal data, or third-party code you do not have the
right to share in a report.
