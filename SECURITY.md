# Security policy

## Reporting a vulnerability

Please report privately rather than in a public issue: open a
[security advisory](https://github.com/minhnd410/computer-control/security/advisories/new)
on this repository.

Include what an attacker gains, the platform and version, and the smallest
reproduction you have. A proof-of-concept is welcome but not required — a clear
description of the mechanism is enough to start.

This is a spare-time project, so expect a first response in days rather than
hours. I will tell you what I think the impact is and when a fix is likely, and
credit you in the advisory unless you would rather I did not.

## Threat model

Be clear about what this software is: **a library that can do anything the
person sitting at the machine can do.** It moves the mouse, types, reads the
screen, and on request runs shell commands. That is the feature. A report that
amounts to "the shell tool runs shell commands" is working as designed.

What *is* in scope:

- **Privilege escalation beyond the invoking user.** Anything that lets the
  library act with rights the process was not granted — bypassing macOS TCC,
  Windows UIPI, or a Linux permission boundary.
- **Command or argument injection.** Subprocess arguments are passed as an argv
  vector and never concatenated into a shell string. A device name, package
  name or file path that escapes that is a real bug.
- **Remote reachability.** The HTTP transport binding somewhere it should not,
  the bearer-token check being bypassable, or a request being served without
  authentication when a token is configured.
- **Memory safety** in the C ABI or the image codecs. The PNG, JPEG and inflate
  implementations parse untrusted bytes: device screenshots come from `adb` and
  `simctl`, and a crafted image reaching a decoder is a legitimate attack path.
- **Policy bypass.** `--no-shell`, `--no-clipboard` and the absence of
  `--allow-registry` are security controls. A tool call that works around one
  is a vulnerability.
- **Secret leakage.** Credentials, clipboard contents or screen contents
  reaching a log, an error message, or a crash dump.

What is **out of scope**:

- The library doing what it was asked to do by an authorized caller.
- An MCP client being tricked by its own model into calling a destructive tool.
  Deciding which tools to expose is the operator's job, which is what `--tools`
  and `--no-shell` are for.
- Needing elevated privileges to drive elevated windows. That is the operating
  system's boundary working correctly.
- Physical access to an unlocked machine.

## Operating it safely

- Bind the HTTP transport to loopback. Binding elsewhere without
  `CC_AUTH_TOKEN` prints a warning, and it means what it says: anything that
  can reach the port controls the machine.
- Give an untrusted client the narrowest surface that works —
  `--tools capabilities,displays,screenshot,snapshot,zoom` is a useful
  read-mostly starting point.
- For genuinely untrusted automation, run it against a display the host does
  not share — a separate user session, a VM, or a dedicated machine. There is
  no in-process sandbox: a tool that can drive the desktop can drive every
  application on it, and no flag changes that.
- Never commit screenshots. Captures routinely contain password managers,
  private messages and customer data; `.gitignore` excludes images by default
  for that reason.

## Supported versions

Pre-1.0: fixes land on `main` and in the next release. There are no maintained
release branches yet.
