# mgit

mgit is a small subset of git written in C, with no library beyond the C library. It reads and writes git's own
on-disk format (SHA-1 objects, loose and packed, the index, refs, packed-refs, the reflog, config), so a repository
mgit makes is an ordinary git repository and git can take over at any point; mgit reads what git writes in the same
way. It covers the local work of one person on one branch at a time, and moving that work to and from a server over
ssh, https or a local path. This is mgit 6.1.1; `mgit --version` prints a build's version.

## What it does

    init [<dir>]                       an empty repository on branch main
    status                             changes since HEAD (what commit would record)
    diff [--quiet | --name-only | --stat] [<rev> <rev>] [-- <path>...]
    commit -m <msg> [-m <msg>...] [--author 'Name <email>'] [-q] [-- <path>...]
    log [--all] [--first-parent] [-<n>] [--since=<date>] [--until=<date>] [--format=<fmt>] [--stat]
        [<rev>...] [-- <path>...]      history in git log's order (%H %h %an %ad %s %n in <fmt>)
    ls-files [-z]                      the files the index tracks
    config [--global | --local] --get <name>
    branch [<name> [<start>]]          list or create branches
    checkout (<branch> | <commit>)     switch, keeping local changes the switch does not touch; a branch only a
                                       remote has is made, tracking the remote's
    tag [-a] [-m <msg>...] <name> [<rev>]
    remote [-v] | remote add <name> <url>
    push [--dry-run] [<remote> [<refspec>...]]
    check [<rev>]                      the pre-push check alone
    clone <url> [<dir>]
    fetch [<remote>]
    pull [<remote> [<branch>]]         fetch, then fast-forward only
    gc [-q]                            pack the loose objects the refs reach into one pack
    rev-parse [--short[=<n>]] <rev>...
    write-tree, hash-object [-w], cat-file (-t | -s | -e | -p | <type>)

`mgit` with no arguments prints the full usage.

- `mgit -C <dir>` runs as if started in that directory, as git's does.
- History and diffs come out as git's: log walks every parent newest first (or `--first-parent`), simplifies by
  paths as git does, and reads `--since` and `--until` as git reads @<seconds>, '<n> <unit>s ago', now, yesterday
  and YYYY-MM-DD [HH:MM[:SS]] [zone] (other forms are refused, where git would take them as now). Diffs are git's
  own algorithm (xdiff's Myers with its cost cutoffs, then its sliding and indent heuristic), so a patch or a
  diffstat is git's byte for byte; there is no rename detection, as with git's `--no-renames`.
- `config --get` reads the repository's config, the home directory's .gitconfig and $XDG_CONFIG_HOME/git/config (no system file and no
  includes, as everywhere mgit reads config); it never writes.
- There is no staging area: `commit` records every file that is not ignored, as `git add -A` followed by
  `git commit` would; with paths, only those paths, and the rest as in HEAD.
- Ignore rules are git's: `.gitignore` in every directory, `.git/info/exclude` and `core.excludesFile`.
- Identities follow git's rules: `GIT_AUTHOR_*` and `GIT_COMMITTER_*`, then `author.*`, `committer.*` and `user.*` in
  the config files git reads, then `$EMAIL`, trimmed as git trims them.
- Objects are compressed with mgit's own deflate and SHA-1 (with the x86 SHA instructions where the processor has
  them). Packs are read by mgit's own code (idx v2, pack v2 and v3, offset and reference deltas, any number of packs)
  and written by it: gc writes a pack v2 and its idx v2, push streams a pack with deltas (on objects in the same pack,
  and on the far side's copies at the same paths, a thin pack as git push sends), and fetch checks every object of the
  pack it receives and writes its idx.
- Push runs `ssh HOST git-receive-pack 'PATH'`, so a host alias in your ssh config works as it does for git; what the
  far side's hooks say comes back as `remote:` lines (side-band-64k). Clone and fetch run `git-upload-pack` over ssh,
  or `curl` for https (smart HTTP, anonymous only); a local path needs neither.

## The pre-push check

Every `mgit push` first checks what it would send, before it connects, and sends nothing if any rule fails; `mgit
check [<rev>]` runs the same check alone. The rules are in a plain text file read again on every run: `$XDG_CONFIG_HOME/mgit/check.conf`
(`.config/mgit/check.conf` in your home directory when `XDG_CONFIG_HOME` is unset) if it exists, else the shipped
`check.conf` as installed. The shipped file refuses keys and tokens by their published shapes, passwords written into
code or urls, email addresses other than placeholders and GitHub's private addresses, branches other than `main`, tags
other than `vX.Y.Z`, and files over GitHub's 100 MiB limit. It explains each kind of rule and carries commented
examples for names, paths and addresses of your own. A file of your own replaces the shipped one, so start from a copy
of it.

Beside the rules, every check also requires that each commit and tag names a person with an email, that each message
is a subject line and then a blank line before any body, that nothing a `.gitignore` ignores is committed, and that
text files have LF line endings. A binary file (a NUL in its first 8000 bytes, as git decides) gets the size rules
and its path only, and a file over the size limit is refused without its text being read. It warns about a Git LFS
pointer and a submodule entry, whose content a push does not carry.

## What it refuses or leaves out

- Force pushes, deleting remote refs, and moving a tag that already exists on the far side.
- Non-fast-forward pulls and pushes: there is no merge and no rebase.
- SHA-256 repositories.
- https with credentials: a url with a user name or password is refused.
- Not built: shallow clones, thin packs on fetch, protocol v2, following redirects, automatic gc, submodule fetch, LFS and
  attributes (a clone that meets submodules, LFS pointers or `.gitattributes` says so), idx v1, alternates and
  promisor clones. Packs over 2 GiB are untested.

## Build

    cc -std=c23 -O2 -ffp-contract=off -Wall -Wextra -o mgit src/*.c

The compiler must accept `-std=c23`. The check's rules use glibc's regular expressions (`\b` among them). Where the
shipped `check.conf` is read from is set at build time, `/usr/local/share/mgit/check.conf` by default:

    cc -std=c23 -O2 -ffp-contract=off -DMGIT_CHECK_DEFAULT='"/usr/share/mgit/check.conf"' -o mgit src/*.c

To install, copy `mgit` to a directory on your `PATH` and `check.conf` to that path.

## Test

    ./test.sh

The suite builds `./mgit`, then holds it against git in temporary repositories: the same changes committed by both
tools must give identical object ids, each tool must read what the other wrote, and `git fsck` must accept what mgit
writes. It needs git itself as the reference (`/usr/bin/git`, with `git-upload-pack` and `git-receive-pack`) and the
usual POSIX tools. The network parts run against stand-in `ssh` and `curl` scripts that serve one made-up host from
local repositories and refuse every other host, so nothing leaves the machine. `MGIT=/path/to/mgit ./test.sh` tests
another build; `MGIT_TEST_ROWS=1` also prints every passing row. It ends with `pass N  fail 0`; on Linux with git
2.56 it runs 791 checks in about 90 seconds (one of them builds a small driver around `src/pack.c`, so it needs a C
compiler as the build does).
