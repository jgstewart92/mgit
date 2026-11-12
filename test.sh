#!/bin/sh
# Checks mgit against /usr/bin/git. Builds ./mgit first unless MGIT names a binary to test.
set -u
here=$(cd "$(dirname "$0")" && pwd)
GIT=/usr/bin/git
if [ -z "${MGIT:-}" ]; then
	cc -std=c23 -O2 -ffp-contract=off -Wall -Wextra -o "$here/mgit" "$here"/src/*.c || exit 1
	MGIT=$here/mgit
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mgit-test.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

export HOME="$tmp/fakehome" XDG_CONFIG_HOME="$tmp/fakehome/.config"
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
# git 2.55 repacks in the background after commits; that races the fixtures' own gc and repack
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=maintenance.auto GIT_CONFIG_VALUE_0=false
export GIT_AUTHOR_NAME='A U Thor' GIT_AUTHOR_EMAIL=author@example.com
export GIT_COMMITTER_NAME='C O Mitter' GIT_COMMITTER_EMAIL=committer@example.com
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY
mkdir -p "$tmp/fakehome"
cd "$tmp" || exit 1

pass=0 fail=0
# MGIT_TEST_ROWS=1 also prints each passing row, so a mutant run can be matched row by row
ok() { pass=$((pass + 1)); [ -z "${MGIT_TEST_ROWS:-}" ] || printf 'PASS %s\n' "${1:-}"; }
bad() { fail=$((fail + 1)); printf 'FAIL %s\n' "$*"; }
# WANT must be non-empty, so two failing commands never count as agreeing.
same() { if [ -n "$2" ] && [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want [$2] got [$3]"; fi; }
same_files() { if [ -s "$2" ] && cmp -s "$2" "$3"; then ok "$1"; else bad "$1"; diff "$2" "$3" | head -10; fi; }
fails() { name=$1; shift; if "$@" >/dev/null 2>&1; then bad "$name: expected failure"; else ok "$name"; fi; }
mg() { timeout 60 "$MGIT" "$@"; }
in_mg() { (cd "$1" && shift && mg "$@"); }
in_git() { (cd "$1" && shift && "$GIT" "$@"); }
objpath() { printf '%s/.git/objects/%s/%s' "$1" "$(printf %s "$2" | cut -c1-2)" "$(printf %s "$2" | cut -c3-)"; }

# 1. blob ids
f=$tmp/files
mkdir -p "$f"
: > "$f/empty"
printf 'hello\n' > "$f/hello"
printf 'a\000b\377\n' > "$f/binary"
head -c 70000 /dev/urandom > "$f/random70k"
seq 1 200000 > "$f/seq"
for x in empty hello binary random70k seq; do
	same "hash-object $x" "$($GIT hash-object "$f/$x")" "$(mg hash-object "$f/$x")"
done
same "hash-object --stdin" "$(printf abc | $GIT hash-object --stdin)" "$(printf abc | mg hash-object --stdin)"

# 2. tree ids for the same working tree
fx=$tmp/fixture
mkdir -p "$fx/foo" "$fx/bin" "$fx/empty" "$fx/nested-empty/sub" "$fx/deep/a/b/c"
cd "$fx" || exit 1
printf 'alpha\n' > a.txt
printf 'dot\n' > foo.c    # sorts before directory foo, because '.' < '/'
printf 'dash\n' > foo-bar
printf 'in dir\n' > foo/x
printf 'zero\n' > foo0    # sorts after directory foo, because '/' < '0'
printf 'space\n' > 'sp ace.txt'
printf 'utf8\n' > 'uni-é.txt'
printf 'tab\n' > "$(printf 'tab\tname')"
printf 'q\n' > 'quote"back\slash'
printf '#!/bin/sh\necho hi\n' > bin/run.sh
chmod 755 bin/run.sh
ln -s a.txt link
ln -s nowhere dangling
printf 'deep\n' > deep/a/b/c/d.txt
printf 'hidden\n' > .hidden
printf 'make\n' > Makefile # byte order puts uppercase before lowercase: Makefile < a.txt
printf 'main\n' > main.c
printf 'Z\n' > Zeta
cp "$f/random70k" big.bin
cd "$tmp" || exit 1

m=$tmp/m g=$tmp/g
cp -a "$fx" "$m"
cp -a "$fx" "$g"
mg init "$m" >/dev/null || bad "mgit init"
$GIT init -q -b main "$g"
same "write-tree" "$(in_git "$g" add -A && in_git "$g" write-tree)" "$(in_mg "$m" write-tree)"

# 3. commit ids: the same changes committed by both tools must give identical commits
commit_both() {
	in_mg "$m" commit "$@" >/dev/null || bad "mgit commit $*"
	{ in_git "$g" add -A && in_git "$g" commit -q "$@"; } || bad "git commit $*"
	same "commit $*" "$(in_git "$g" rev-parse HEAD)" "$(in_mg "$m" rev-parse HEAD)"
}
both() { (cd "$m" && eval "$1") && (cd "$g" && eval "$1"); }

export GIT_AUTHOR_DATE='1790000000 +0200' GIT_COMMITTER_DATE='1790000100 -0530'
commit_both -m first
both 'printf "more\n" >> a.txt && rm foo0 link && ln -s foo.c link && printf "new\n" > foo/new && chmod 644 bin/run.sh'
export GIT_AUTHOR_DATE='1791028800 +0000' GIT_COMMITTER_DATE='1791028800 +0000'  # single-digit day
commit_both -m 'second subject' -m "$(printf 'body one\na\tb')"
both 'printf "3\n" > three'
export GIT_AUTHOR_DATE='@1791032400 +1345' GIT_COMMITTER_DATE='1791032400 -1100'
commit_both -m "$(printf '\n\n  indented subject   \n\t\n\n\nlast line  \n\n')"
in_mg "$m" commit -m again >/dev/null 2>&1
same "unchanged tree: commit exits 1" 1 $?

# 4. git reads what mgit wrote
out=$(in_git "$m" fsck --strict --no-dangling 2>&1)
st=$?
if [ $st -eq 0 ] && [ -z "$out" ]; then ok; else bad "git fsck --strict on mgit repo ($st): $out"; fi
in_git "$m" log > "$tmp/want.log"
in_mg "$m" log > "$tmp/got.log"
same_files "log output matches git log" "$tmp/want.log" "$tmp/got.log"
same "log shows 3 commits" 3 "$(grep -c '^commit ' "$tmp/got.log")"
in_git "$m" cat-file blob HEAD:big.bin > "$tmp/big"
same_files "git reads a two-block stored object" "$fx/big.bin" "$tmp/big"

# 5. mgit reads what git wrote (real zlib, several compression levels)
for lvl in 0 1 9; do
	seq "$lvl" 150000 > "$f/seq$lvl"
	$GIT -C "$g" -c core.looseCompression=$lvl hash-object -w "$f/seq$lvl" >/dev/null
done
head -c 100000 /dev/urandom > "$f/rnd"
$GIT -C "$g" -c core.looseCompression=9 hash-object -w "$f/rnd" >/dev/null
[ -z "$(ls "$g/.git/objects/pack")" ] || bad "git repo has packfiles, so the loose-object comparison is void"
in_git "$g" cat-file --batch-all-objects --batch-check='%(objectname)' > "$tmp/objects"
n=0
while read -r o; do
	n=$((n + 1))
	in_git "$g" cat-file -p "$o" > "$tmp/want" || bad "git cat-file -p $o"
	if in_mg "$g" cat-file -p "$o" > "$tmp/got" && cmp -s "$tmp/want" "$tmp/got"; then ok; else bad "cat-file -p $o"; fi
	same "cat-file -t $o" "$(in_git "$g" cat-file -t "$o")" "$(in_mg "$g" cat-file -t "$o")"
done < "$tmp/objects"
if [ "$n" -ge 30 ]; then ok; else bad "only $n git objects compared"; fi

# 6. revisions and branches
short=$(in_git "$m" rev-parse --short=7 HEAD~1)
for r in HEAD HEAD~1 HEAD^ HEAD~2 HEAD^1 HEAD~0 HEAD^0 HEAD~1^ main refs/heads/main "$short"; do
	same "rev-parse $r" "$(in_git "$m" rev-parse "$r")" "$(in_mg "$m" rev-parse "$r")"
done
fails "rev-parse past the root commit" in_mg "$m" rev-parse HEAD~3
fails "rev-parse unknown name" in_mg "$m" rev-parse nosuch
fails "rev-parse path escape" in_mg "$m" rev-parse ../../etc/passwd
in_mg "$m" branch feature HEAD~1 || bad "branch feature HEAD~1"
in_mg "$m" branch topic/nested || bad "branch topic/nested"
same "branch start point" "$(in_git "$m" rev-parse HEAD~1)" "$(in_git "$m" rev-parse feature)"
same "branch at HEAD" "$(in_git "$m" rev-parse HEAD)" "$(in_git "$m" rev-parse topic/nested)"
fails "branch that already exists" in_mg "$m" branch feature
for name in 'bad..name' '-x' 'a b' 'x.lock' '.dot' 'end/' 'a@{b' 'HEAD'; do
	fails "branch name '$name' rejected" in_mg "$m" branch "$name"
done
same "only valid refs created" 3 "$(in_git "$m" for-each-ref | wc -l)"
in_git "$m" branch > "$tmp/want"
in_mg "$m" branch > "$tmp/got"
same_files "branch list" "$tmp/want" "$tmp/got"

# 7. packed-refs, as left behind by git gc or git pack-refs
in_git "$m" pack-refs --all
[ ! -e "$m/.git/refs/heads/main" ] || bad "pack-refs left main loose, so the packed-refs test is void"
same "rev-parse packed main" "$(in_git "$m" rev-parse main)" "$(in_mg "$m" rev-parse main)"
in_git "$m" branch > "$tmp/want"
in_mg "$m" branch > "$tmp/got"
same_files "branch list from packed-refs" "$tmp/want" "$tmp/got"
prev=$(in_git "$m" rev-parse HEAD)
printf 'four\n' > "$m/four"
export GIT_AUTHOR_DATE='1791036000 +0200' GIT_COMMITTER_DATE='1791036000 +0200'
in_mg "$m" commit -m 'fourth, onto a packed ref' >/dev/null || bad "commit onto packed ref"
same "parent is the packed tip" "$prev" "$(in_git "$m" rev-parse HEAD~1)"
same "git sees the new tip" "$(in_mg "$m" rev-parse HEAD)" "$(in_git "$m" rev-parse main)"

# 8. a held ref lock stops the update
before=$(in_git "$m" rev-parse main)
mkdir -p "$m/.git/refs/heads"
: > "$m/.git/refs/heads/main.lock"
printf 'five\n' > "$m/five"
fails "commit while the ref is locked" in_mg "$m" commit -m locked
same "locked ref unchanged" "$before" "$(in_git "$m" rev-parse main)"
rm -f "$m/.git/refs/heads/main.lock" "$m/five"

# 9. corrupt objects are refused (git fsck confirms each copy really is corrupt)
a=$(in_git "$m" rev-parse HEAD:a.txt)
b=$(in_git "$m" rev-parse HEAD:foo.c)
for kind in flip swap truncate; do
	c=$tmp/c-$kind
	cp -a "$m" "$c"
	p=$(objpath "$c" "$a")
	chmod u+w "$p"
	case $kind in
	flip) printf Z | dd of="$p" bs=1 seek=$(($(stat -c %s "$p") - 5)) conv=notrunc 2>/dev/null ;;
	swap) cp -f "$(objpath "$c" "$b")" "$p" ;;
	truncate) truncate -s 10 "$p" ;;
	esac
	fails "git fsck sees the $kind corruption" in_git "$c" fsck
	fails "mgit refuses the $kind object" in_mg "$c" cat-file -p "$a"
done

# ---- v2: index, ignore rules, status, diff, checkout, push and pull ----
# The command must succeed and print nothing.
quiet() { name=$1; shift; out=$("$@" 2>&1); st=$?; if [ $st -eq 0 ] && [ -z "$out" ]; then ok "$name"; else bad "$name ($st): $(printf %s "$out" | head -5)"; fi; }
tab=$(printf '\t')
clean='nothing to commit, working tree clean'

# 10. git trusts the index mgit writes, and mgit reads the index git writes
# diff-files runs first: git status refreshes the index and would hide bad stat data
quiet "git diff-files accepts mgit's stat data" in_git "$m" diff-files
quiet "git status clean after an mgit commit" in_git "$m" status --porcelain
quiet "mgit's index equals HEAD" in_git "$m" diff-index --cached HEAD
same "mgit reads git's index" "$clean" "$(in_mg "$g" status | tail -n 1)"

# 11. status, diff and commit agree with git add -A, including ignore rules
v=$tmp/v2
mkdir -p "$v/src"
cd "$v" || exit 1
{
	printf '#include <stdio.h>\n\nstatic int helper(int x)\n{\n'
	for i in 1 2 3 4 5 6 7 8 9 10 11 12; do printf '\tint v%d = x + %d;\n' $i $i; done
	printf '\treturn x;\n}\n\nint main(void)\n{\n'
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do printf '\tprintf("line %d\\n");\n' $i; done
	printf '\treturn helper(0);\n}\n'
} > src/code.c
printf 'one\ntwo\nthree\n' > notes.txt
printf 'a\nb' > tail.txt
printf 'echo mode\n' > mode.sh
printf 'echo both\n' > both.sh
printf 'bye\n' > gone.txt
printf 'swap me\n' > swap
printf 'bin\000ary\n' > bin.dat
printf 'space\n' > 'sp ace.txt'
printf 'utf8\n' > 'uni-é.txt'
printf 'log\n' > tracked.log
ln -s notes.txt link
cd "$tmp" || exit 1
v2_changes() {
	# helper: changes 7 lines apart (two hunks); main: 6 lines apart (one hunk), git's merge boundary
	sed -i 's/int v3 = x + 3;/int v3 = x * 3;/; s/int v11 = x + 11;/int v11 = x * 11;/; s/printf("line 9\\n");/printf("line nine\\n");/' src/code.c
	awk '{ print } /line 15\\n/ { print "\tprintf(\"inserted\\n\");" }' src/code.c > src/code.new && mv src/code.new src/code.c
	printf 'four' >> notes.txt
	printf '\n' >> tail.txt
	chmod 755 mode.sh both.sh
	printf 'echo more\n' >> both.sh
	rm gone.txt link swap
	ln -s src/code.c link
	ln -s notes.txt swap
	: > empty-new
	printf 'bin\000ary2\n' > bin.dat
	printf 'new\000bin\n' > newbin
	printf 'space two\n' >> 'sp ace.txt'
	printf 'utf8 two\n' >> 'uni-é.txt'
	printf '*.o\n/build/\n!keep.o\ndocs/**/draft.md\n*.log\ncache/\n' > .gitignore
	mkdir -p cache sub
	printf 'c\n' > cache/x
	printf 'c\n' > sub/cache
	printf 'x\n' > x.o
	printf 'k\n' > keep.o
	mkdir -p build sub/deeper docs/a/b
	printf 'b\n' > build/out
	printf 'd\n' > docs/draft.md
	printf 'd\n' > docs/a/b/draft.md
	printf 'r\n' > docs/readme.md
	printf 'local.txt\n' > sub/.gitignore
	printf 'l\n' > sub/local.txt
	printf 'l\n' > sub/deeper/local.txt
	printf 'k\n' > sub/keep.txt
	printf 'more log\n' >> tracked.log
	printf 'o\n' > other.log
	printf 's\n' > secret.txt
	printf 'w\n' > x.swp
}
m=$tmp/s g=$tmp/t # the v1 repositories are finished; commit_both and both act on these from here on
cp -a "$v" "$m"
cp -a "$v" "$g"
mg init "$m" >/dev/null || bad "mgit init"
$GIT init -q -b main "$g"
mkdir -p "$m/.git/info" "$XDG_CONFIG_HOME/git"
printf 'secret.txt\n' > "$m/.git/info/exclude"
printf 'secret.txt\n' >> "$g/.git/info/exclude"
printf '*.swp\n' > "$XDG_CONFIG_HOME/git/ignore"
export GIT_AUTHOR_DATE='1791100000 +0100' GIT_COMMITTER_DATE='1791100000 +0100'
commit_both -m 'v2 base'
both v2_changes
in_git "$g" add -A
in_git "$g" diff --cached --name-status --no-renames HEAD > "$tmp/want"
in_mg "$m" status | tail -n +2 > "$tmp/got"
same_files "status matches git add -A" "$tmp/want" "$tmp/got"
same "status header" "On branch main" "$(in_mg "$m" status | head -n 1)"
for p in x.o build/out docs/draft.md docs/a/b/draft.md sub/local.txt sub/deeper/local.txt other.log secret.txt x.swp cache/x; do
	if grep -q "$tab$p\$" "$tmp/got"; then bad "status lists ignored $p"; else ok; fi
done
for p in keep.o sub/keep.txt docs/readme.md tracked.log .gitignore sub/.gitignore sub/cache; do
	if grep -q "$tab$p\$" "$tmp/got"; then ok; else bad "status misses $p"; fi
done
in_git "$g" diff --cached --no-renames HEAD > "$tmp/want"
in_mg "$m" diff > "$tmp/got"
same_files "diff matches git diff" "$tmp/want" "$tmp/got"
commit_both -m 'v2 changes'
quiet "git diff-files clean after the v2 commit" in_git "$m" diff-files
quiet "git status clean after the v2 commit" in_git "$m" status --porcelain
same "mgit status clean after the v2 commit" "$clean" "$(in_mg "$m" status | tail -n 1)"

# 12. diffs are minimal (against an LCS computed in awk) and reverse cleanly with git apply -R
lcs_cost() {
	awk 'NR == FNR { a[++n] = $0; next } { b[++m] = $0 }
	END {
		for (i = 0; i <= n; i++) L[i, 0] = 0
		for (j = 0; j <= m; j++) L[0, j] = 0
		for (i = 1; i <= n; i++)
			for (j = 1; j <= m; j++)
				L[i, j] = a[i] == b[j] ? L[i - 1, j - 1] + 1 : (L[i - 1, j] > L[i, j - 1] ? L[i - 1, j] : L[i, j - 1])
		print n + m - 2 * L[n, m]
	}' "$1" "$2"
}
for seed in 1 2 3; do
	awk -v s=$seed 'BEGIN { srand(s); for (i = 0; i < 300; i++) print substr("aabbc", int(rand() * 5) + 1, 1) }' > "$m/rep$seed.txt"
done
in_mg "$m" commit -m 'repetitive files' >/dev/null || bad "commit repetitive files"
for seed in 1 2 3; do
	cp "$m/rep$seed.txt" "$tmp/old$seed"
	awk -v s=$((seed + 10)) 'BEGIN { srand(s) } { r = rand(); if (r < 0.1) next; print; if (r > 0.9) print substr("abcd", int(rand() * 4) + 1, 1) }' \
		"$tmp/old$seed" > "$m/rep$seed.txt"
done
rm "$m/notes.txt" "$m/link"
printf 'was a link\n' > "$m/link"
printf 'n\n' > "$m/fresh.txt"
chmod 644 "$m/both.sh"
in_mg "$m" diff > "$tmp/patch"
for seed in 1 2 3; do
	got=$(awk -v f="a/rep$seed.txt" '$1 == "diff" { on = $3 == f; next } on && /^[-+]/ && !/^(---|\+\+\+) / { c++ } END { print c + 0 }' "$tmp/patch")
	same "rep$seed.txt diff is minimal" "$(lcs_cost "$tmp/old$seed" "$m/rep$seed.txt")" "$got"
done
r=$tmp/random
mkdir -p "$r/repo"
mg init "$r/repo" >/dev/null
bad_min=0 bad_rev=0 i=0
while [ $i -lt 300 ] && [ $((bad_min + bad_rev)) -eq 0 ]; do
	i=$((i + 1))
	awk -v s=$i 'BEGIN { srand(s); n = int(rand() * 60) + 1; for (k = 0; k < n; k++) print substr("aab", int(rand() * 3) + 1, 1) }' > "$r/repo/f"
	in_mg "$r/repo" commit -m base >/dev/null 2>&1
	cp "$r/repo/f" "$r/old"
	awk -v s=$((i + 100000)) 'BEGIN { srand(s) } { x = rand(); if (x < 0.3) next; print; if (x > 0.7) print substr("abc", int(rand() * 3) + 1, 1) } END { if (rand() < 0.5) print "a" }' "$r/old" > "$r/repo/f"
	[ -s "$r/repo/f" ] || echo a > "$r/repo/f"
	cp "$r/repo/f" "$r/new"
	in_mg "$r/repo" diff > "$r/p"
	got=$(awk '/^[-+]/ && !/^(---|\+\+\+) / { c++ } END { print c + 0 }' "$r/p")
	[ "$got" = "$(lcs_cost "$r/old" "$r/new")" ] || bad_min=$((bad_min + 1))
	if [ -s "$r/p" ]; then (cd "$r/repo" && $GIT apply -R "$r/p") 2>/dev/null; fi
	cmp -s "$r/repo/f" "$r/old" || bad_rev=$((bad_rev + 1))
	cp "$r/new" "$r/repo/f"
done
same "random diffs are minimal (stops at the first bad case of 300)" 0 "$bad_min"
same "random diffs reverse with git apply -R" 0 "$bad_rev"
cp -a "$m" "$tmp/rt"
(cd "$tmp/rt" && $GIT apply -R "$tmp/patch") || bad "git apply -R of mgit's patch"
same "the patch reverses to HEAD" "$clean" "$(in_mg "$tmp/rt" status | tail -n 1)"
in_mg "$m" commit -m 'edits' >/dev/null || bad "commit edits"

# 13. checkout
c1=$(in_mg "$m" rev-parse HEAD)
in_mg "$m" branch side || bad "branch side"
printf 'main only\n' > "$m/main-only.txt"
rm "$m/tail.txt"
printf 'changed\n' >> "$m/src/code.c"
mkdir "$m/fd"
printf 'inner\n' > "$m/fd/inner"
in_mg "$m" commit -m 'main moves on' >/dev/null || bad "commit on main"
in_mg "$m" checkout side >/dev/null || bad "checkout side"
same "HEAD follows side" refs/heads/side "$(in_git "$m" symbolic-ref HEAD)"
same "side is at the branch point" "$c1" "$(in_git "$m" rev-parse HEAD)"
quiet "git diff-files clean on side" in_git "$m" diff-files
quiet "git status clean on side" in_git "$m" status --porcelain
if [ ! -e "$m/main-only.txt" ] && [ -f "$m/tail.txt" ] && [ ! -e "$m/fd" ]; then ok; else bad "side's files not in place"; fi
printf 'fd is a file here\n' > "$m/fd"
printf 'side\n' > "$m/side-only.txt"
in_mg "$m" commit -m 'side work' >/dev/null || bad "commit on side"
in_mg "$m" checkout main >/dev/null || bad "checkout main (file fd becomes directory fd/)"
if [ -f "$m/fd/inner" ] && [ ! -e "$m/side-only.txt" ]; then ok; else bad "main's files not in place"; fi
quiet "git status clean on main" in_git "$m" status --porcelain
in_mg "$m" checkout side >/dev/null || bad "checkout side (directory fd/ becomes file fd)"
if [ -f "$m/fd" ]; then ok; else bad "fd is not a file on side"; fi
in_mg "$m" checkout main >/dev/null || bad "checkout main again"
printf 'local edit\n' >> "$m/sp ace.txt"
in_mg "$m" checkout side >/dev/null || bad "checkout with an unrelated local change"
same "an unrelated local change is carried over" "M${tab}sp ace.txt" "$(in_mg "$m" status | tail -n +2)"
in_mg "$m" checkout main >/dev/null || bad "checkout main with the local change"
in_git "$m" checkout -q -- 'sp ace.txt'
printf 'mine\n' >> "$m/src/code.c"
before=$(cksum < "$m/src/code.c")
fails "checkout refuses to overwrite a local change" in_mg "$m" checkout side
same "HEAD unchanged after the refusal" refs/heads/main "$(in_git "$m" symbolic-ref HEAD)"
same "the local change is kept" "$before" "$(cksum < "$m/src/code.c")"
in_git "$m" checkout -q -- src/code.c
printf 'in the way\n' > "$m/side-only.txt"
fails "checkout refuses to overwrite an untracked file" in_mg "$m" checkout side
same "the untracked file is kept" "in the way" "$(cat "$m/side-only.txt")"
rm "$m/side-only.txt"
mkdir "$m/sl"
printf 'x\n' > "$m/sl/x"
in_mg "$m" commit -m 'add sl/x' >/dev/null || bad "commit sl/x"
in_mg "$m" checkout side >/dev/null || bad "checkout side before the symlink test"
mkdir "$tmp/outside"
ln -s "$tmp/outside" "$m/sl"
fails "checkout refuses to write through a symlinked directory" in_mg "$m" checkout main
same "the refused checkout changed nothing" "A${tab}sl" "$(in_mg "$m" status | tail -n +2)"
if [ -z "$(ls -A "$tmp/outside")" ]; then ok; else bad "checkout wrote outside the working tree"; fi
rm "$m/sl"
in_mg "$m" checkout main >/dev/null || bad "checkout main after the symlink test"
in_mg "$m" checkout HEAD~1 >/dev/null || bad "detached checkout"
same "detached at main~1" "$(in_git "$m" rev-parse main~1)" "$(in_git "$m" rev-parse HEAD)"
fails "HEAD is detached" in_git "$m" symbolic-ref -q HEAD
quiet "git status clean when detached" in_git "$m" status --porcelain
in_mg "$m" checkout main >/dev/null || bad "checkout main after detaching"

# 14. push and pull, fast-forward only
b=$tmp/bare.git
$GIT init -q --bare "$b"
in_mg "$m" push "$b" main >/dev/null || bad "push to a bare repository"
same "the bare repository has main" "$(in_mg "$m" rev-parse main)" "$($GIT -C "$b" rev-parse main)"
quiet "git fsck on the pushed repository" $GIT -C "$b" fsck --strict --no-dangling
mg init "$tmp/c" >/dev/null || bad "mgit init c"
in_mg "$tmp/c" pull "$b" main >/dev/null || bad "pull into an empty repository"
same "pull checked out main" "$(in_mg "$m" rev-parse main)" "$(in_mg "$tmp/c" rev-parse HEAD)"
quiet "git diff-files clean after the pull" in_git "$tmp/c" diff-files
quiet "git status clean after the pull" in_git "$tmp/c" status --porcelain
quiet "index equals HEAD after the pull" in_git "$tmp/c" diff-index --cached HEAD
same "mgit status clean after the pull" "$clean" "$(in_mg "$tmp/c" status | tail -n 1)"
printf 'more\n' >> "$m/fresh.txt"
in_mg "$m" commit -m 'for the push' >/dev/null || bad "commit for the push"
in_mg "$m" push "$b" >/dev/null || bad "fast-forward push"
in_mg "$tmp/c" pull "$b" >/dev/null || bad "fast-forward pull"
same "pull fast-forwarded" "$(in_mg "$m" rev-parse main)" "$(in_mg "$tmp/c" rev-parse HEAD)"
same "pulled content" "$(cat "$m/fresh.txt")" "$(cat "$tmp/c/fresh.txt")"
printf 'c side\n' > "$tmp/c/c-only.txt"
in_mg "$tmp/c" commit -m 'c diverges' >/dev/null || bad "commit in c"
printf 'm side\n' > "$m/m-only.txt"
in_mg "$m" commit -m 'm diverges' >/dev/null || bad "commit in m"
in_mg "$m" push "$b" >/dev/null || bad "push m's divergent commit"
before=$($GIT -C "$b" rev-parse main)
fails "non-fast-forward push rejected" in_mg "$tmp/c" push "$b"
same "bare repository unchanged by the rejected push" "$before" "$($GIT -C "$b" rev-parse main)"
fails "pull refuses diverged history" in_mg "$tmp/c" pull "$b"
case $(in_mg "$tmp/c" push "$m" main 2>&1) in
*"checked out"*) ok ;;
*) bad "push to a checked-out branch was not refused as such" ;;
esac
mg init "$tmp/d" >/dev/null || bad "mgit init d"
in_mg "$tmp/d" pull "$b" >/dev/null || bad "pull into d"
printf 'm again\n' >> "$m/m-only.txt"
in_mg "$m" commit -m 'm again' >/dev/null || bad "commit m again"
in_mg "$m" push "$b" >/dev/null || bad "push m again"
printf 'd edit\n' >> "$tmp/d/m-only.txt"
fails "pull refuses to overwrite a local change" in_mg "$tmp/d" pull "$b"
same "the local change survives the refused pull" "d edit" "$(tail -n 1 "$tmp/d/m-only.txt")"

# ---- v3: reading packs ----
# 15. repositories git has packed: OFS_DELTA (gc), REF_DELTA (repack without offsets), and a clone from a bundle
pk=$tmp/packsrc
mkdir -p "$pk"
$GIT init -q -b main "$pk"
seq 1 30000 > "$pk/big.txt" # over 64 KiB, so deltas carry copy instructions of the 0x10000 default size
printf 'bin\000ary\n' > "$pk/bin.dat"
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	sed -i "${i}s/.*/changed $i/" "$pk/big.txt"
	printf 'line %d\n' $i >> "$pk/small.txt"
	[ $i -eq 8 ] && $GIT -C "$pk" branch side
	$GIT -C "$pk" add -A
	$GIT -C "$pk" commit -q -m "commit $i"
done
$GIT -C "$pk" checkout -q side
printf 'side\n' > "$pk/side.txt"
$GIT -C "$pk" add -A
$GIT -C "$pk" commit -q -m 'side work'
$GIT -C "$pk" checkout -q main
$GIT -C "$pk" merge -q --no-ff -m 'merge side' side
$GIT -C "$pk" tag -a -m 'a tag' v1 HEAD~2
# Counts the pack entries of a type (6 OFS_DELTA, 7 REF_DELTA) from each entry's first byte.
pack_types() {
	p=$(ls "$1"/.git/objects/pack/*.pack)
	$GIT verify-pack -v "${p%.pack}.idx" | awk '$2 ~ /^(commit|tree|blob|tag)$/ { print $5 }' |
		while read -r off; do echo $(( ($(od -An -tu1 -j "$off" -N1 "$p") >> 4) & 7 )); done | grep -c "^$2\$"
}
cp -a "$pk" "$tmp/ofs"
$GIT -C "$tmp/ofs" gc -q --aggressive
cp -a "$pk" "$tmp/ref"
$GIT -C "$tmp/ref" -c repack.useDeltaBaseOffset=false repack -q -a -d -f
$GIT -C "$pk" bundle create -q "$tmp/all.bundle" --all
$GIT clone -q "$tmp/all.bundle" "$tmp/cloned"
# several packs and a multi-pack-index, the layout git's own maintenance leaves
cp -a "$pk" "$tmp/multi"
$GIT -C "$tmp/multi" repack -q -d
$GIT -C "$tmp/multi" checkout -q -b extra
printf 'more\n' >> "$tmp/multi/small.txt"
$GIT -C "$tmp/multi" commit -q -am 'after the first pack'
$GIT -C "$tmp/multi" checkout -q main
$GIT -C "$tmp/multi" repack -q -d
$GIT -C "$tmp/multi" multi-pack-index write
same "multi: two packs" 2 "$(ls "$tmp/multi"/.git/objects/pack/*.pack | wc -l)"
for v in ofs ref cloned multi; do
	same "$v: every object packed" "count: 0" "$($GIT -C "$tmp/$v" count-objects -v | grep '^count:')"
done
if [ "$(pack_types "$tmp/ofs" 6)" -gt 0 ]; then ok; else bad "gc made no OFS_DELTA entries, so that test is void"; fi
if [ "$(pack_types "$tmp/ref" 7)" -gt 0 ]; then ok; else bad "repack made no REF_DELTA entries, so that test is void"; fi
for v in ofs ref cloned multi; do
	r=$tmp/$v
	n=0 badn=0
	for o in $(in_git "$r" cat-file --batch-all-objects --batch-check='%(objectname)'); do
		n=$((n + 1))
		in_git "$r" cat-file -p "$o" > "$tmp/want"
		if ! in_mg "$r" cat-file -p "$o" > "$tmp/got" 2>&1 || ! cmp -s "$tmp/want" "$tmp/got" ||
			[ "$(in_git "$r" cat-file -t "$o")" != "$(in_mg "$r" cat-file -t "$o")" ]; then
			badn=$((badn + 1))
		fi
	done
	same "$v: all $n packed objects read as git reads them" 0 "$badn"
	if [ "$n" -ge 60 ]; then ok; else bad "$v: only $n objects"; fi
	in_git "$r" log --first-parent > "$tmp/want"
	in_mg "$r" log > "$tmp/got"
	same_files "$v: log matches git log --first-parent" "$tmp/want" "$tmp/got"
	short=$(in_git "$r" rev-parse --short=7 HEAD~4)
	for rev in HEAD HEAD^2 v1 "$short"; do
		same "$v: rev-parse $rev" "$(in_git "$r" rev-parse "$rev")" "$(in_mg "$r" rev-parse "$rev")"
	done
	same "$v: status on git's checkout" "$clean" "$(in_mg "$r" status | tail -n 1)"
	in_mg "$r" cat-file -e "$(in_git "$r" rev-parse HEAD~5)"
	same "$v: cat-file -e finds a packed object" 0 $?
done
in_git "$tmp/cloned" branch > "$tmp/want"
in_mg "$tmp/cloned" branch > "$tmp/got"
same_files "cloned: branch list" "$tmp/want" "$tmp/got"

# 16. writing on top of packs: checkout, commit, pull out of and push from a packed repository
r=$tmp/cloned
in_git "$r" branch -q side origin/side
in_mg "$r" checkout side >/dev/null || bad "checkout side in a packed clone"
quiet "packed: git diff-files clean on side" in_git "$r" diff-files
quiet "packed: git status clean on side" in_git "$r" status --porcelain
in_mg "$r" checkout main >/dev/null || bad "checkout main in a packed clone"
printf 'mgit was here\n' >> "$r/small.txt"
in_mg "$r" commit -m 'loose on top of a pack' >/dev/null || bad "commit in a packed clone"
out=$(in_git "$r" fsck --strict --no-dangling 2>&1)
if [ $? -eq 0 ] && [ -z "$out" ]; then ok; else bad "packed: git fsck after an mgit commit: $out"; fi
mg init "$tmp/fresh" >/dev/null || bad "mgit init fresh"
in_mg "$tmp/fresh" pull "$tmp/ofs" main >/dev/null || bad "pull out of a packed repository"
same "pull from packs checked out main" "$(in_git "$tmp/ofs" rev-parse main)" "$(in_mg "$tmp/fresh" rev-parse HEAD)"
out=$(in_git "$tmp/fresh" fsck --strict --no-dangling 2>&1)
if [ $? -eq 0 ] && [ -z "$out" ]; then ok; else bad "fsck after pulling from packs: $out"; fi
$GIT init -q --bare "$tmp/bare2.git"
in_mg "$r" push "$tmp/bare2.git" main >/dev/null || bad "push from a packed clone"
quiet "fsck of a repository pushed from packs" $GIT -C "$tmp/bare2.git" fsck --strict --no-dangling
$GIT -C "$tmp/ofs" worktree add -q "$tmp/wt" side 2>/dev/null
in_git "$tmp/ofs" branch > "$tmp/want"
in_mg "$tmp/ofs" branch > "$tmp/got"
same_files "branch marks a branch checked out in a linked worktree" "$tmp/want" "$tmp/got"
if grep -q '^+ side$' "$tmp/got"; then ok; else bad "no worktree branch in the fixture"; fi
# far more delta bases than the 256 cache slots, all read by one checkout
mr=$tmp/many
$GIT init -q -b main "$mr"
awk -v d="$mr" 'BEGIN { for (i = 0; i < 600; i++) { f = d "/f" i; for (j = 0; j < 20; j++) print "file " i " line " j > f; close(f) } }'
$GIT -C "$mr" add -A
$GIT -C "$mr" commit -q -m 'many files'
awk -v d="$mr" 'BEGIN { for (i = 0; i < 600; i++) { f = d "/f" i; for (j = 0; j < 20; j++) print (j == 10 ? "edited " i : "file " i " line " j) > f; close(f) } }'
$GIT -C "$mr" commit -q -am 'many edits'
$GIT -C "$mr" gc -q --aggressive || bad "gc of the many-files repository"
p=$(ls "$mr"/.git/objects/pack/*.pack)
deltas=$($GIT verify-pack -v "${p%.pack}.idx" | awk 'NF == 7' | wc -l)
if [ "$deltas" -ge 512 ]; then ok; else bad "only $deltas deltas, too few to share cache slots"; fi
in_mg "$mr" checkout HEAD~1 >/dev/null || bad "checkout of 600 deltified files"
mkdir "$tmp/many-want"
$GIT -C "$mr" archive HEAD | tar -xmf - -C "$tmp/many-want"
if diff -r --exclude=.git "$tmp/many-want" "$mr" >/dev/null; then ok; else bad "files rebuilt from deltas differ from git archive"; fi
in_mg "$mr" checkout main >/dev/null || bad "checkout main of 600 files"

# 17. corrupt packs are refused (git fsck confirms each copy is corrupt)
p=$(ls "$tmp/ofs"/.git/objects/pack/*.pack)
$GIT verify-pack -v "${p%.pack}.idx" > "$tmp/vp"
plain=$(awk 'NF == 5 && $2 == "blob" { print $1; exit }' "$tmp/vp")
deltified=$(awk 'NF == 7 && $2 == "blob" { print $1; exit }' "$tmp/vp")
base=$(awk -v d="$deltified" '$1 == d { print $7 }' "$tmp/vp")
for kind in plain base; do
	c=$tmp/pc-$kind
	cp -a "$tmp/ofs" "$c"
	cp_pack=$(ls "$c"/.git/objects/pack/*.pack)
	chmod u+w "$cp_pack"
	if [ $kind = plain ]; then target=$plain read_id=$plain; else target=$base read_id=$deltified; fi
	set -- $(awk -v t="$target" '$1 == t { print $5, $4 }' "$tmp/vp")
	printf '\377' | dd of="$cp_pack" bs=1 seek=$(($1 + $2 - 3)) conv=notrunc 2>/dev/null
	fails "git fsck sees the corrupt $kind entry" in_git "$c" fsck
	fails "mgit refuses an object whose $kind entry is corrupt" in_mg "$c" cat-file -p "$read_id"
done
# an index entry pointing at another blob: the data inflates cleanly, so only the object hash can tell
c=$tmp/pc-idx
cp -a "$tmp/ofs" "$c"
idx=$(ls "$c"/.git/objects/pack/*.idx)
chmod u+w "$idx"
$GIT -C "$c" show-index < "$idx" > "$tmp/si"
other=$(awk -v p="$plain" 'NF == 5 && $2 == "blob" && $1 != p { print $1; exit }' "$tmp/vp")
k=$(awk -v p="$plain" '$2 == p { print NR - 1 }' "$tmp/si")
off=$(awk -v o="$other" '$2 == o { print $1 }' "$tmp/si")
printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((off >> 24 & 255)) $((off >> 16 & 255)) $((off >> 8 & 255)) $((off & 255)))" |
	dd of="$idx" bs=1 seek=$((8 + 1024 + $(wc -l < "$tmp/si") * 24 + 4 * k)) conv=notrunc 2>/dev/null
fails "git fsck sees the redirected index entry" in_git "$c" fsck
fails "mgit refuses a packed object whose content has another id" in_mg "$c" cat-file -p "$plain"


# ---- v4: path-limited commits, diff --quiet, --name-only and --stat, rev-parse --short ----
# 18. commit -- PATH...: HEAD's tree with the named paths as the working tree has them, everything else left alone
m=$tmp/pm g=$tmp/pg # commit_both and both act on these from here on
mkdir -p "$m/keep/sub" "$m/other"
cd "$m" || exit 1
printf 'a\n' > keep/a.txt
printf 'b\n' > keep/sub/b.txt
printf 'gone\n' > keep/gone.txt
printf 'gone2\n' > keep/sub/gone2.txt
printf '*.tmp\n' > keep/.gitignore
printf 'top\n' > top.txt
mkdir keep-out # shares a prefix with keep but is not inside it
printf 'ko\n' > keep-out/x.txt
printf 'c\n' > other/c.txt
printf 'd\n' > other/d.txt
printf 'notes\n' > notes.md
printf 'a file for now\n' > fd
printf 'echo\n' > mode.sh
cd "$tmp" || exit 1
cp -a "$m" "$g"
mg init "$m" >/dev/null || bad "mgit init pm"
$GIT init -q -b main "$g"
export GIT_AUTHOR_DATE='1792000000 +0000' GIT_COMMITTER_DATE='1792000000 +0000'
commit_both -m 'v4 base'
both 'printf "a2\n" >> keep/a.txt && rm keep/gone.txt keep/sub/gone2.txt && printf "new\n" > keep/new.txt && printf "t\n" > keep/x.tmp && chmod 755 keep/sub/b.txt && printf "top2\n" >> top.txt && rm other/c.txt && printf "n\n" > other/new.txt && printf "notes2\n" >> notes.md && rm fd && mkdir fd && printf "inner\n" > fd/inner && chmod 755 mode.sh && printf "ko2\n" >> keep-out/x.txt'
export GIT_AUTHOR_DATE='1792000100 +0000' GIT_COMMITTER_DATE='1792000200 +0000'
au='P A Thor <pa@example.com>'
out=$(in_mg "$m" commit -q --author "$au" -m 'named paths' -- keep notes.md fd/inner 2>&1) || bad "mgit commit -- keep notes.md fd/inner"
{ in_git "$g" add -- keep notes.md fd/inner && in_git "$g" commit -q --author "$au" -m 'named paths' -- keep notes.md fd/inner; } || bad "git commit -- keep notes.md fd/inner"
same "path-limited commit: the commit git makes" "$(in_git "$g" rev-parse HEAD)" "$(in_mg "$m" rev-parse HEAD)"
same "path-limited commit: --author sets the author, not the committer" "P A Thor <pa@example.com> C O Mitter" "$(in_git "$m" log -1 --format='%an <%ae> %cn')"
[ -z "$out" ] && ok "commit -q prints nothing" || bad "commit -q printed: $out"
out=$(in_git "$m" fsck --strict --no-dangling 2>&1)
if [ $? -eq 0 ] && [ -z "$out" ]; then ok "git fsck --strict accepts a path-limited commit"; else bad "git fsck after a path-limited commit: $out"; fi
in_git "$g" diff-files --name-status > "$tmp/want"
in_git "$m" diff-files --name-status > "$tmp/got"
same_files "path-limited commit: git diff-files sees what it sees after git's (index updated for the named paths only)" "$tmp/want" "$tmp/got"
in_git "$g" status --porcelain > "$tmp/want"
in_git "$m" status --porcelain > "$tmp/got"
same_files "path-limited commit: git status as after git's" "$tmp/want" "$tmp/got"
same "path-limited commit: the other changes stay uncommitted" " D other/c.txt| M keep-out/x.txt| M mode.sh| M top.txt|?? other/new.txt" "$(LC_ALL=C sort "$tmp/got" | paste -sd'|' -)"
fails "path-limited commit: a deletion in a named directory is recorded" in_git "$m" cat-file -e HEAD:keep/gone.txt
fails "path-limited commit: a deletion in a named directory's subdirectory is recorded" in_git "$m" cat-file -e HEAD:keep/sub/gone2.txt
same "path-limited commit: a new file in a named directory is added" "new" "$(in_git "$m" show HEAD:keep/new.txt)"
fails "path-limited commit: an ignored file in a named directory stays out" in_git "$m" cat-file -e HEAD:keep/x.tmp
same "path-limited commit: a mode change in a named directory is recorded" 100755 "$(in_git "$m" ls-tree HEAD keep/sub/b.txt | cut -c1-6)"
same "path-limited commit: a changed file outside the paths keeps HEAD's content" "$(in_git "$m" rev-parse HEAD~1:top.txt)" "$(in_git "$m" rev-parse HEAD:top.txt)"
same "path-limited commit: a deleted file outside the paths stays in the tree" "$(in_git "$m" rev-parse HEAD~1:other/c.txt)" "$(in_git "$m" rev-parse HEAD:other/c.txt)"
same "path-limited commit: a file replaced by a directory, as git add replaces it" "tree inner" "$(in_git "$m" cat-file -t HEAD:fd) $(in_git "$m" show HEAD:fd/inner)"
same "mgit status lists what the path-limited commit left" "M${tab}keep-out/x.txt|M${tab}mode.sh|D${tab}other/c.txt|A${tab}other/new.txt|M${tab}top.txt" \
	"$(in_mg "$m" status | tail -n +2 | paste -sd'|' -)"
before=$(in_mg "$m" rev-parse HEAD)
in_mg "$m" commit -m again -- keep notes.md >/dev/null 2>&1
same "commit of unchanged paths exits 1 while other paths changed" 1 $?
fails "commit naming a path that matches nothing is refused whole" in_mg "$m" commit -m x -- top.txt nosuch
printf 'w\n' > "$m/w*"
fails "commit of a path with a wildcard is refused, even where a file has that name" in_mg "$m" commit -m x -- 'w*'
rm -f "$m/w*"
fails "commit of a path outside the working tree is refused" in_mg "$m" commit -m x -- ../top.txt
fails "--author without <email> is refused" in_mg "$m" commit --author nobody -m x -- top.txt
same "refused commits leave HEAD alone" "$before" "$(in_mg "$m" rev-parse HEAD)"
export GIT_AUTHOR_DATE='1792000300 +0000' GIT_COMMITTER_DATE='1792000300 +0000'
(cd "$m/other" && mg commit -q -m 'from a subdirectory' -- c.txt ../top.txt) || bad "mgit commit from other/"
(cd "$g/other" && $GIT add -- c.txt ../top.txt && $GIT commit -q -m 'from a subdirectory' -- c.txt ../top.txt) || bad "git commit from other/"
same "paths are read from the current directory, .. included" "$(in_git "$g" rev-parse HEAD)" "$(in_mg "$m" rev-parse HEAD)"
in_git "$g" status --porcelain > "$tmp/want"
in_git "$m" status --porcelain > "$tmp/got"
same_files "git status after the commit from a subdirectory" "$tmp/want" "$tmp/got"
export GIT_AUTHOR_DATE='1792000400 +0000' GIT_COMMITTER_DATE='1792000400 +0000'
commit_both -m 'the rest, all paths'
quiet "git status clean after the whole-tree commit" in_git "$m" status --porcelain

# 19. diff --quiet, --name-only and --stat: git add then git diff --cached, as a script checking staged changes does, on the same trees
diff_cases() {
	sed -i 's/^a2$/a3/' keep/a.txt
	rm keep/sub/b.txt
	printf 'n2\n' >> other/new.txt
	seq 1 300 > keep/sub/long-numbers-file-name-to-push-the-graph.txt
	mkdir -p very/long/directory/name/that/goes/on/and/on
	seq 1 40 > very/long/directory/name/that/goes/on/and/on/numbers-file-with-a-long-name.txt
	printf 'bin\000ary\n' > keep/bin.dat
	printf 'mode\000only\n' > keep/bin2.dat
	printf 'q\n' > 'keep/uni-é.txt'
	: > keep/empty
	chmod 644 mode.sh
	rm notes.md
	ln -s top.txt notes.md
}
both diff_cases
export GIT_AUTHOR_DATE='1792000500 +0000' GIT_COMMITTER_DATE='1792000500 +0000'
commit_both -m 'diff base'
diff_more() {
	seq 1 3 300 > keep/sub/long-numbers-file-name-to-push-the-graph.txt
	seq 5 44 > very/long/directory/name/that/goes/on/and/on/numbers-file-with-a-long-name.txt
	printf 'bin\000ary, longer\n' > keep/bin.dat
	printf 'new\000bin\n' > keep/newbin
	printf 'q2\n' >> 'keep/uni-é.txt'
	rm keep/empty
	chmod 755 mode.sh keep/bin2.dat
	rm notes.md
	printf 'notes again\n' > notes.md
	printf 'top3\n' >> top.txt
	printf 'u\n' > untracked.txt
}
both diff_more
unset COLUMNS
for spec in "" "keep very" "keep/sub/long-numbers-file-name-to-push-the-graph.txt" mode.sh; do
	in_git "$g" add -A -- $spec
	in_git "$g" diff --cached --name-only -- $spec > "$tmp/want"
	in_mg "$m" diff --name-only -- $spec > "$tmp/got"
	same_files "diff --name-only as git's, paths [$spec]" "$tmp/want" "$tmp/got"
	in_git "$g" diff --cached --stat -- $spec > "$tmp/want"
	in_mg "$m" diff --stat -- $spec > "$tmp/got"
	same_files "diff --stat as git's, paths [$spec]" "$tmp/want" "$tmp/got"
	in_git "$g" diff --cached --no-renames -- $spec > "$tmp/want"
	in_mg "$m" diff -- $spec > "$tmp/got"
	same_files "diff patch as git's, paths [$spec]" "$tmp/want" "$tmp/got"
	in_git "$g" diff --cached --quiet -- $spec
	want=$?
	in_mg "$m" diff --quiet -- $spec > "$tmp/got"
	same "diff --quiet exit status as git's, paths [$spec]" "$want" "$?"
	[ -s "$tmp/got" ] && bad "diff --quiet printed something" || ok "diff --quiet prints nothing, paths [$spec]"
	in_git "$g" reset -q
done
same "diff --stat names the binary files, one changed only in mode" 3 "$(in_mg "$m" diff --stat | grep -c -E '[|] +Bin( |$)')"
for cols in 40 50 200; do
	in_git "$g" add -A
	COLUMNS=$cols in_git "$g" diff --cached --stat > "$tmp/want"
	COLUMNS=$cols in_mg "$m" diff --stat > "$tmp/got"
	same_files "diff --stat as git's at COLUMNS=$cols" "$tmp/want" "$tmp/got"
	in_git "$g" reset -q
done
in_mg "$m" diff --quiet -- fd
same "diff --quiet exits 0 for unchanged paths" 0 $?
in_mg "$m" diff --stat -- fd > "$tmp/got"
[ -s "$tmp/got" ] && bad "diff --stat of unchanged paths printed something" || ok "diff --stat of unchanged paths prints nothing"
fails "diff takes one of --quiet, --name-only, --stat" in_mg "$m" diff --stat --name-only
both 'mv very/long/directory/name/that/goes/on/and/on/numbers-file-with-a-long-name.txt moved.txt'
in_git "$g" add -A
in_git "$g" diff --cached --no-renames --stat > "$tmp/want"
in_git "$g" reset -q
in_mg "$m" diff --stat > "$tmp/got"
same_files "no rename detection: a moved file is a deletion and an addition, as git's --no-renames" "$tmp/want" "$tmp/got"
export GIT_AUTHOR_DATE='1792000600 +0000' GIT_COMMITTER_DATE='1792000600 +0000'
commit_both -m 'diff cases'
in_mg "$m" diff --quiet
same "diff --quiet exits 0 on a clean tree" 0 $?
# commit writes git's reflog lines, so HEAD@{n} and HEAD@{<time>} read mgit's commits
grep -v "	reset: moving to HEAD$" "$g/.git/logs/HEAD" > "$tmp/want" # the git side's own resets in section 19
same_files "reflog of HEAD as git writes it, over the v4 commits" "$tmp/want" "$m/.git/logs/HEAD"
same_files "reflog of the branch as git writes it" "$g/.git/logs/refs/heads/main" "$m/.git/logs/refs/heads/main"
same "the reflog has every v4 commit" 6 "$(wc -l < "$m/.git/logs/HEAD")"
same "HEAD@{2} through mgit's reflog" "$(in_git "$m" rev-parse HEAD~2)" "$(in_git "$m" rev-parse 'HEAD@{2}')"
same "HEAD@{<time>} through mgit's reflog, as through git's" "$(in_git "$g" rev-parse 'HEAD@{1792000450}')" "$(in_git "$m" rev-parse 'HEAD@{1792000450}')"
for t in mg git; do
	$GIT init -q -b main "$tmp/nolog-$t"
	$GIT -C "$tmp/nolog-$t" config core.logAllRefUpdates false
	printf 'x\n' > "$tmp/nolog-$t/f"
done
in_mg "$tmp/nolog-mg" commit -q -m x || bad "mgit commit with logAllRefUpdates false"
(cd "$tmp/nolog-git" && $GIT add -A && $GIT commit -q -m x) || bad "git commit with logAllRefUpdates false"
same "core.logAllRefUpdates=false: no reflog made, as git" "absent absent" \
	"$([ -e "$tmp/nolog-git/.git/logs/HEAD" ] && echo present || echo absent) $([ -e "$tmp/nolog-mg/.git/logs/HEAD" ] && echo present || echo absent)"

# 20. rev-parse --short: git's length, which grows with the packed object count, and unique prefixes
same "rev-parse --short HEAD" "$(in_git "$m" rev-parse --short HEAD)" "$(in_mg "$m" rev-parse --short HEAD)"
same "rev-parse --short=12 HEAD~1" "$(in_git "$m" rev-parse --short=12 HEAD~1)" "$(in_mg "$m" rev-parse --short=12 HEAD~1)"
same "rev-parse --short=2 is held at 4" "$(in_git "$m" rev-parse --short=2 HEAD)" "$(in_mg "$m" rev-parse --short=2 HEAD)"
fails "rev-parse --short takes one revision" in_mg "$m" rev-parse --short HEAD HEAD~1
bigp=$tmp/bigpack bigl=$tmp/bigloose
$GIT init -q -b main "$bigp"
awk 'BEGIN { for (i = 1; i <= 17000; i++) { s = "blob " i "\n"; printf "blob\nmark :%d\ndata %d\n%s", i, length(s), s }
	printf "commit refs/heads/main\ncommitter C <c@d> 1792000000 +0000\ndata 4\nbig\nM 100644 :1 f\n" }' | $GIT -C "$bigp" fast-import --quiet
same "17000 objects: all packed" "count: 0" "$($GIT -C "$bigp" count-objects -v | grep '^count:')"
want=$(in_git "$bigp" rev-parse --short HEAD)
same "rev-parse --short over 2^14 packed objects" "$want" "$(in_mg "$bigp" rev-parse --short HEAD)"
same "git abbreviates to 8 over 2^14 packed objects (so the row above can fail)" 8 "${#want}"
$GIT init -q -b main "$bigl"
$GIT -C "$bigl" unpack-objects -q < "$(ls "$bigp"/.git/objects/pack/*.pack)"
$GIT -C "$bigl" update-ref refs/heads/main "$(in_git "$bigp" rev-parse HEAD)"
same "17000 objects: all loose (17000 blobs, a tree, a commit)" "count: 17002" "$($GIT -C "$bigl" count-objects -v | grep '^count:')"
same "rev-parse --short over 17000 loose objects (git counts only packed ones)" "$(in_git "$bigl" rev-parse --short HEAD)" "$(in_mg "$bigl" rev-parse --short HEAD)"
pair=$(in_git "$bigp" cat-file --batch-all-objects --batch-check='%(objectname)' | awk '{ p = substr($1, 1, 5) } p == last { print $1; exit } { last = p }')
same "rev-parse --short=4 of an object sharing 5 digits" "$(in_git "$bigp" rev-parse --short=4 "$pair")" "$(in_mg "$bigp" rev-parse --short=4 "$pair")"
[ "${#pair}" = 40 ] && ok "a 5-digit collision exists among 17000 objects" || bad "no 5-digit collision found, so the row above is void"

# 21. an object a pack already holds is not written again as a loose copy when mgit re-hashes its file
pr=$tmp/packed-rehash
$GIT init -q -b main "$pr"
for i in 1 2 3 4 5; do printf 'file %d\n' $i > "$pr/f$i"; done
$GIT -C "$pr" add -A && $GIT -C "$pr" commit -q -m base && $GIT -C "$pr" gc -q
same "packed-rehash: every object packed" "count: 0" "$($GIT -C "$pr" count-objects -v | grep '^count:')"
for i in 1 2 3 4 5; do cp -p "$pr/f$i" "$pr/f$i.tmp" && mv -f "$pr/f$i.tmp" "$pr/f$i"; done # new inodes: no stat data vouches
printf 'changed\n' >> "$pr/f1"
in_mg "$pr" commit -q -m 'one change' || bad "mgit commit in packed-rehash"
same "a commit over re-hashed packed files writes only its new blob, tree and commit" "count: 3" "$($GIT -C "$pr" count-objects -v | grep '^count:')"

# 22. identities as git writes them (ident.c): crud trimmed, delimiters dropped, empty and missing values, $EMAIL
idn=0
# CONF (both: user.name and user.email; name: user.name only; role: also author.name) LABEL [commit options]; the
# identity comes from the variables the caller exports, after idreset
ident_case() {
	conf=$1 label=$2
	shift 2
	idn=$((idn + 1))
	im=$tmp/ident$idn-mg ig=$tmp/ident$idn-git
	for r in "$im" "$ig"; do
		$GIT init -q -b main "$r"
		$GIT -C "$r" config user.name 'Conf Name'
		[ "$conf" = name ] || $GIT -C "$r" config user.email conf@example.org
		[ "$conf" != role ] || $GIT -C "$r" config author.name 'Role Author'
		printf 'x\n' > "$r/f"
	done
	(cd "$im" && mg commit -q "$@" -m m) >/dev/null 2>&1
	(cd "$ig" && $GIT add -A && $GIT commit -q "$@" -m m) >/dev/null 2>&1
	if $GIT -C "$ig" rev-parse -q --verify HEAD >/dev/null; then
		same "identity: $label" "$($GIT -C "$ig" rev-parse HEAD)" "$($GIT -C "$im" rev-parse -q --verify HEAD)"
	else
		same "identity: $label (git refuses)" "refused" "$($GIT -C "$im" rev-parse -q --verify HEAD >/dev/null && echo committed || echo refused)"
	fi
}
idreset() { unset GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL EMAIL; }
export GIT_AUTHOR_DATE='1792000700 +0000' GIT_COMMITTER_DATE='1792000700 -0130'
idreset; export GIT_AUTHOR_NAME='  .Ann; ' GIT_AUTHOR_EMAIL=' <ann@example.org>, '; ident_case both "crud trimmed from a name and an email"
idreset; export GIT_AUTHOR_NAME='A<b>c' GIT_COMMITTER_NAME="$(printf 'Ne\nwline')"; ident_case both "<, > and a newline dropped from inside a name"
idreset; export GIT_AUTHOR_NAME='"J. R. Doe Jr."' GIT_AUTHOR_EMAIL="'j@example.org'"; ident_case both "quotes trimmed, dots kept"
idreset; export GIT_COMMITTER_EMAIL=' <c@example.org> ' GIT_COMMITTER_NAME='Zoë Ñandú'; ident_case both "a UTF-8 committer name, crud around its email"
idreset; ident_case both "--author with blanks around it" --author '  Bo Dee   <bo@example.org>  '
idreset; ident_case both "--author with an empty email" --author 'Cy <>'
idreset; ident_case both "--author with crud in both parts" --author ', Di; <;di@example.org;>'
idreset; export GIT_AUTHOR_NAME=; ident_case both "GIT_AUTHOR_NAME set but empty"
idreset; export GIT_AUTHOR_EMAIL=; ident_case both "GIT_AUTHOR_EMAIL set but empty"
idreset; export GIT_AUTHOR_NAME=' ;; '; ident_case both "a name of crud only"
idreset; ident_case role "author.name for the author only"
idreset; export EMAIL=env@example.org; ident_case name "no user.email: \$EMAIL"
idreset; ident_case name "no user.email and no \$EMAIL (git's own guess is refused on a host without a domain)"
idreset; export GIT_AUTHOR_NAME='A U Thor' GIT_AUTHOR_EMAIL=author@example.com GIT_COMMITTER_NAME='C O Mitter' GIT_COMMITTER_EMAIL=committer@example.com


# ---- v5: deflate, packs written by gc, tags, remotes, push over ssh, the pre-push check ----
# 23. objects are deflated with Huffman coding, and git (zlib) reads every one back
z=$tmp/z zf=$tmp/zfiles
mg init "$z" >/dev/null || bad "mgit init z"
mkdir -p "$zf"
: > "$zf/empty"
printf a > "$zf/one"
head -c 1000000 /dev/zero > "$zf/zeros"
head -c 70000 /dev/urandom > "$zf/random"
seq 1 200000 > "$zf/seq"
yes 'abcabcabd' | head -c 300000 > "$zf/pattern"
head -c 40000 /dev/urandom > "$tmp/r1"
head -c 40000 /dev/urandom > "$tmp/r2"
cat "$tmp/r1" "$tmp/r2" "$tmp/r1" "$tmp/r1" "$tmp/r2" > "$zf/far" # repeats beyond the 32 KiB window and inside it
{ head -c 258 /dev/zero; printf x; head -c 259 /dev/zero; printf y; head -c 516 /dev/zero; } > "$zf/runs" # the 258-byte match limit
cat "$here"/src/*.c > "$zf/source"
awk 'BEGIN { srand(7); for (i = 0; i < 200000; i++) printf "%c", 65 + int(rand() * rand() * 26) }' > "$zf/skewed"
cp "$MGIT" "$zf/program" # an ELF program: its Huffman codes need the 15-bit limit
zg=$tmp/zg
$GIT init -q "$zg"
msize=0 gsize=0
for x in empty one zeros random seq pattern far runs source skewed program; do
	id=$(in_mg "$z" hash-object -w "$zf/$x")
	same "git reads mgit's deflate: $x" "$(cksum < "$zf/$x")" "$(in_git "$z" cat-file blob "$id" > "$tmp/zout" && cksum < "$tmp/zout")"
	same "mgit reads its own deflate: $x" "$(cksum < "$zf/$x")" "$(in_mg "$z" cat-file blob "$id" > "$tmp/zout" && cksum < "$tmp/zout")"
	$GIT -C "$zg" -c core.looseCompression=9 hash-object -w "$zf/$x" >/dev/null
	msize=$((msize + $(stat -c %s "$(objpath "$z" "$id")"))) gsize=$((gsize + $(stat -c %s "$(objpath "$zg" "$id")")))
done
out=$(in_git "$z" fsck --strict --no-dangling 2>&1)
if [ $? -eq 0 ] && [ -z "$(printf '%s\n' "$out" | grep -v '^notice: No default references')" ]; then ok "git fsck --strict on mgit's deflated objects"; else bad "git fsck on deflated objects: $out"; fi
id=$(in_mg "$z" hash-object "$zf/seq")
if [ "$(stat -c %s "$(objpath "$z" "$id")")" -lt $(($(stat -c %s "$zf/seq") * 2 / 5)) ]; then ok "loose objects are deflated: the seq file in under 40 %"; else bad "the seq object is $(stat -c %s "$(objpath "$z" "$id")") bytes"; fi
if [ $((msize * 100)) -le $((gsize * 102)) ]; then ok "deflate within 2 % of zlib -9 over the inputs"; else bad "deflate: mgit $msize bytes, zlib -9 $gsize"; fi
id=$(in_mg "$z" hash-object "$zf/random")
if [ "$(stat -c %s "$(objpath "$z" "$id")")" -le 70064 ]; then ok "an incompressible file is stored: 70 KB of random bytes in at most 64 bytes more"; else bad "the random object is $(stat -c %s "$(objpath "$z" "$id")") bytes"; fi
id=$(in_mg "$z" hash-object "$zf/source")
a=$(stat -c %s "$(objpath "$z" "$id")") b=$(stat -c %s "$(objpath "$zg" "$id")")
if [ $((a * 100)) -le $((b * 101)) ]; then ok "deflate within 1 % of zlib -9 on mgit's own C source"; else bad "deflate on the source: mgit $a bytes, zlib -9 $b"; fi

# 24. gc: one pack and idx v2 of the loose objects the refs reach; git verify-pack, index-pack, fsck and clone accept them
pg=$tmp/gc
mg init "$pg" >/dev/null || bad "mgit init gc"
mkdir -p "$pg/d/e"
seq 1 30000 > "$pg/big.txt"
printf 'bin\000ary\n' > "$pg/bin.dat"
printf 'deep\n' > "$pg/d/e/f"
ln -s big.txt "$pg/link"
printf '#!/bin/sh\n' > "$pg/run.sh"
chmod 755 "$pg/run.sh"
for i in 1 2 3 4 5; do
	sed -i "${i}s/.*/changed $i/" "$pg/big.txt"
	printf 'line %d\n' $i >> "$pg/d/e/f"
	in_mg "$pg" commit -q -m "gc commit $i" || bad "gc fixture commit $i"
done
in_mg "$pg" branch side HEAD~2
in_mg "$pg" tag -a -m 'a release' v1.0.0 HEAD~1
in_mg "$pg" tag light HEAD~3
reach=$(in_git "$pg" rev-list --objects --all | wc -l)
in_mg "$pg" gc >/dev/null || bad "mgit gc"
same "gc: one pack" 1 "$(ls "$pg"/.git/objects/pack/*.pack 2>/dev/null | wc -l)"
same "gc: no loose object left (all were reachable)" "count: 0" "$(in_git "$pg" count-objects -v | grep '^count:')"
same "gc: the pack holds every reachable object" "in-pack: $reach" "$(in_git "$pg" count-objects -v | grep '^in-pack:')"
p=$(ls "$pg"/.git/objects/pack/*.pack)
if $GIT verify-pack -v "${p%.pack}.idx" > "$tmp/vp5" 2>&1; then ok "gc: git verify-pack accepts mgit's pack and idx"; else bad "gc: git verify-pack: $(tail -n 3 "$tmp/vp5")"; fi
same "gc: every entry whole (no deltas)" "non delta: $reach objects" "$(grep '^non delta' "$tmp/vp5")"
mkdir -p "$tmp/rebuilt"
cp "$p" "$tmp/rebuilt/x.pack"
name=$($GIT index-pack -o "$tmp/rebuilt/x.idx" "$tmp/rebuilt/x.pack" 2>/dev/null)
same_files "gc: git index-pack rebuilds mgit's idx byte for byte" "${p%.pack}.idx" "$tmp/rebuilt/x.idx"
same "gc: the pack is named by its checksum, as git names packs" "pack-$name.pack" "$(basename "$p")"
quiet "gc: git fsck --strict after gc" in_git "$pg" fsck --strict --no-dangling
n=0 badn=0
for o in $(in_git "$pg" cat-file --batch-all-objects --batch-check='%(objectname)'); do
	n=$((n + 1))
	in_git "$pg" cat-file -p "$o" > "$tmp/want"
	if ! in_mg "$pg" cat-file -p "$o" > "$tmp/got" 2>&1 || ! cmp -s "$tmp/want" "$tmp/got"; then badn=$((badn + 1)); fi
done
same "gc: mgit reads every object back from its pack" "$reach 0" "$n $badn"
in_git "$pg" log > "$tmp/want"
in_mg "$pg" log > "$tmp/got"
same_files "gc: log after gc matches git log" "$tmp/want" "$tmp/got"
same "gc: mgit status clean after gc" "$clean" "$(in_mg "$pg" status | tail -n 1)"
quiet "gc: git status clean after gc" in_git "$pg" status --porcelain
$GIT clone -q --no-local "$pg" "$tmp/gc-clone" 2>/dev/null || bad "git clone of an mgit-packed repository"
same "gc: git clone of the packed repository: HEAD" "$(in_git "$pg" rev-parse HEAD)" "$(in_git "$tmp/gc-clone" rev-parse HEAD)"
same "gc: git clone of the packed repository: tags" "$(in_git "$pg" show-ref --tags)" "$(in_git "$tmp/gc-clone" show-ref --tags)"
if diff -r --exclude=.git "$pg" "$tmp/gc-clone" >/dev/null; then ok "gc: the clone's files are the repository's"; else bad "gc: the clone's files differ"; fi
same "gc: nothing loose: no new pack" "nothing to pack: no loose object is reachable" "$(in_mg "$pg" gc)"
orphan=$(printf 'nobody points here\n' | in_mg "$pg" hash-object -w --stdin)
printf 'six\n' > "$pg/six"
in_mg "$pg" commit -q -m 'after the pack' || bad "commit after gc"
in_mg "$pg" gc >/dev/null || bad "second gc"
same "gc: a second pack for what came after the first" 2 "$(ls "$pg"/.git/objects/pack/*.pack | wc -l)"
same "gc: the second pack holds the 3 new objects" "in-pack: $((reach + 3))" "$(in_git "$pg" count-objects -v | grep '^in-pack:')"
if [ -f "$(objpath "$pg" "$orphan")" ]; then ok "gc: an unreachable loose object is left alone"; else bad "gc removed an unreachable loose object"; fi
quiet "gc: git fsck --strict after the second gc" in_git "$pg" fsck --strict --no-dangling
printf 'seven\n' > "$pg/seven"
in_mg "$pg" commit -q -m seven || bad "commit seven"
before=$(in_git "$pg" count-objects -v | grep '^count:')
chmod 555 "$pg/.git/objects/pack"
fails "gc: refused when the pack cannot be written" in_mg "$pg" gc
chmod 755 "$pg/.git/objects/pack"
same "gc: the refused gc kept every loose object" "$before" "$(in_git "$pg" count-objects -v | grep '^count:')"
quiet "gc: the repository is whole after the refused gc" in_git "$pg" fsck --strict --no-dangling
cp -a "$pg" "$tmp/gc-twin"
in_mg "$tmp/gc-twin" gc >/dev/null || bad "gc in the twin"
newidx=none
for q in "$tmp/gc-twin"/.git/objects/pack/*.idx; do [ -e "$pg/.git/objects/pack/${q##*/}" ] || newidx=${q##*/}; done
mkdir "$pg/.git/objects/pack/$newidx" # the same objects make the same pack, so its idx name is known: a directory there blocks it
fails "gc: refused when its idx cannot take its place" in_mg "$pg" gc
rmdir "$pg/.git/objects/pack/$newidx"
same "gc: refused after writing its pack, it kept every loose object" "$before" "$(in_git "$pg" count-objects -v | grep '^count:')"
same "gc: and it left no pack without an idx" "garbage: 0" "$(in_git "$pg" count-objects -v | grep '^garbage:')"
quiet "gc: the repository is whole after that refusal" in_git "$pg" fsck --strict --no-dangling
before=$(in_git "$tmp/cloned" count-objects -v | awk '$1 == "count:" { c = $2 } $1 == "in-pack:" { p = $2 } END { print "in-pack: " c + p }')
in_mg "$tmp/cloned" gc >/dev/null || bad "gc on a clone git packed"
same "gc on top of git's pack: each loose object packed once, the packed ones left" "$before" "$(in_git "$tmp/cloned" count-objects -v | grep '^in-pack:')"
same "gc on top of git's pack: nothing loose left" "count: 0" "$(in_git "$tmp/cloned" count-objects -v | grep '^count:')"
quiet "gc on top of git's pack: git fsck --strict" in_git "$tmp/cloned" fsck --strict --no-dangling

# 25. tags: lightweight and annotated, the same objects git tag writes
tg=$tmp/tg tgg=$tmp/tgg
mg init "$tg" >/dev/null || bad "mgit init tg"
$GIT init -q -b main "$tgg"
for i in 1 2; do
	printf '%d\n' $i > "$tg/a"
	printf '%d\n' $i > "$tgg/a"
	in_mg "$tg" commit -q -m "tagged $i"
	(cd "$tgg" && $GIT add -A && $GIT commit -q -m "tagged $i")
done
same "tag: the same commits on both sides" "$(in_git "$tgg" rev-parse HEAD)" "$(in_git "$tg" rev-parse HEAD)"
tag_both() {
	label=$1 name=$2
	shift 2
	in_mg "$tg" tag "$@" >/dev/null 2>&1
	in_git "$tgg" tag "$@" >/dev/null 2>&1
	same "tag: $label" "$($GIT -C "$tgg" rev-parse -q --verify "refs/tags/$name")" "$($GIT -C "$tg" rev-parse -q --verify "refs/tags/$name")"
}
tag_both "lightweight at HEAD" v0.1.0 v0.1.0
tag_both "lightweight at HEAD~1" v0.0.9 v0.0.9 HEAD~1
tag_both "annotated with -a -m" v1.0.0 -a -m 'Release 1.0.0' v1.0.0
tag_both "-m alone annotates" v1.0.1 -m 'one more' v1.0.1
tag_both "two -m paragraphs" v1.0.2 -m first -m second v1.0.2
tag_both "comment lines and blank runs cleaned as git tag cleans them" v1.0.3 -m "$(printf 'subj\n# comment\n  \n\n\nbody  \n#x')" v1.0.3
tag_both "an empty message" v1.0.4 -m '' v1.0.4
tag_both "a tag of a tag" v1.0.5 -a -m nested v1.0.5 v1.0.0
tag_both "annotated at HEAD~1" v1.0.6 -m old v1.0.6 HEAD~1
in_git "$tgg" tag > "$tmp/want"
in_mg "$tg" tag > "$tmp/got"
same_files "tag: the list as git tag lists it" "$tmp/want" "$tmp/got"
before=$(in_git "$tg" rev-parse v1.0.0)
fails "tag: an existing name is refused" in_mg "$tg" tag v1.0.0 HEAD~1
same "tag: the refused tag is unchanged" "$before" "$(in_git "$tg" rev-parse v1.0.0)"
fails "tag: an invalid name is refused" in_mg "$tg" tag 'bad..name'
fails "tag: -a without a message is refused" in_mg "$tg" tag -a v2.0.0
quiet "tag: git fsck --strict accepts mgit's tags" in_git "$tg" fsck --strict --no-dangling

# 26. remotes: the config git writes and reads
rmr=$tmp/rm rgr=$tmp/rg
mg init "$rmr" >/dev/null || bad "mgit init rm"
$GIT init -q "$rgr"
cp "$rmr/.git/config" "$rgr/.git/config"
remote_both() {
	in_mg "$rmr" remote add "$1" "$2" >/dev/null 2>&1
	m_st=$?
	in_git "$rgr" remote add "$1" "$2" >/dev/null 2>&1
	same "remote add: $3" "exit $?" "exit $m_st"
}
remote_both origin ssh://host.example/repo.git "an ssh url"
remote_both odd 'h:a b#c;d' "a url with a space, # and ;"
remote_both esc 'x\y"z' "a url with a backslash and a quote"
remote_both lead ' lead' "a url starting with a space"
remote_both origin elsewhere "an existing remote, refused with git's exit 3"
remote_both 'bad name' x "an invalid name, refused"
same_files "remote add: the config file git writes, byte for byte" "$rgr/.git/config" "$rmr/.git/config"
same "remote: git reads back the odd url" 'h:a b#c;d' "$(in_git "$rmr" config remote.odd.url)"
in_git "$rgr" config remote.odd.pushurl 'p:q'
cp "$rgr/.git/config" "$rmr/.git/config"
in_git "$rgr" remote > "$tmp/want"
in_mg "$rmr" remote > "$tmp/got"
same_files "remote: the names, sorted as git remote lists them" "$tmp/want" "$tmp/got"
in_git "$rgr" remote -v > "$tmp/want"
in_mg "$rmr" remote -v > "$tmp/got"
same_files "remote -v as git shows it, pushurl included" "$tmp/want" "$tmp/got"

# 27. push over ssh to git-receive-pack, against a bare repository here. The harness's ssh runs the command on this
# machine for the host "standin" and refuses every other host, so nothing can leave.
fb=$tmp/fakebin
mkdir -p "$fb" "$XDG_CONFIG_HOME/mgit"
cat > "$fb/ssh" <<'EOF'
#!/bin/sh
[ "$1" = -p ] && shift 2
case $1 in standin | someone@standin) ;; *) echo "test ssh: refusing host $1" >&2; exit 255 ;; esac
printf '%s\n' "$2" >> "${0%/*}/commands"
exec sh -c "$2"
EOF
chmod 755 "$fb/ssh"
export PATH="$fb:$PATH"
# The suite's rules: the shipped ones, plus private rules of the kind a site adds for its own paths, addresses, host
# names, session names and stores, with generic stand-ins that section 28 plants
testconf=$tmp/check-test.conf
{ cat "$here/check.conf"; cat <<'EOF'; } > "$testconf"

# test only
private /(home|Users)/
private [~]/
private \b10\.[0-9]+\.[0-9]+\.[0-9]+\b
private \b(hostA|hostB)\b
private \bsession-[a-z]+\b
private \bprivate-store\b
EOF
cp "$testconf" "$XDG_CONFIG_HOME/mgit/check.conf"
ps_=$tmp/ps bare=$tmp/remote.git
mg init "$ps_" >/dev/null || bad "mgit init ps"
$GIT init -q --bare -b main "$bare"
$GIT -C "$bare" config receive.fsckObjects true # git fsck checks every object it receives, as GitHub does
printf 'hello\n' > "$ps_/README"
mkdir -p "$ps_/src"
seq 1 5000 > "$ps_/src/data.txt"
in_mg "$ps_" commit -q -m 'Start' -m 'The first commit.' || bad "push fixture commit"
in_mg "$ps_" remote add origin "standin:$bare"
out=$(in_mg "$ps_" push 2>&1)
same "push: a new branch" "$(in_mg "$ps_" rev-parse main)" "$($GIT -C "$bare" rev-parse -q --verify main)"
case $out in *"[new branch]"*"main -> main"*) ok "push: reports the new branch" ;; *) bad "push output: $out" ;; esac
grep -qxF "git-receive-pack '$bare'" "$fb/commands" && ok "push: runs git-receive-pack 'PATH' on the far side" || bad "push ran: $(cat "$fb/commands")"
same "push: the remote-tracking ref follows" "$(in_mg "$ps_" rev-parse main)" "$(in_git "$ps_" rev-parse -q --verify refs/remotes/origin/main)"
printf 'more\n' >> "$ps_/README"
in_mg "$ps_" commit -q -m 'More' || bad "push fixture commit 2"
before=$($GIT -C "$bare" rev-parse main)
out=$(in_mg "$ps_" push --dry-run 2>&1)
same "push --dry-run sends nothing" "$before" "$($GIT -C "$bare" rev-parse main)"
case $out in *"would send 3 objects"*) ok "push --dry-run says what it would send" ;; *) bad "push --dry-run: $out" ;; esac
in_mg "$ps_" push >/dev/null 2>&1 || bad "fast-forward push"
same "push: a fast-forward" "$(in_mg "$ps_" rev-parse main)" "$($GIT -C "$bare" rev-parse main)"
same "push: up to date sends nothing" "Everything up to date" "$(in_mg "$ps_" push 2>&1 | tail -n 1)"
fails "push: a force refspec is refused" in_mg "$ps_" push origin +main
in_mg "$ps_" tag -a -m 'Version 1.0.0' v1.0.0
in_mg "$ps_" tag v1.0.1 HEAD~1
in_mg "$ps_" push origin v1.0.0 v1.0.1 >/dev/null 2>&1 || bad "push two tags"
same "push: an annotated tag by name" "$(in_git "$ps_" rev-parse v1.0.0)" "$($GIT -C "$bare" rev-parse -q --verify refs/tags/v1.0.0)"
same "push: a lightweight tag by name" "$(in_git "$ps_" rev-parse v1.0.1)" "$($GIT -C "$bare" rev-parse -q --verify refs/tags/v1.0.1)"
in_mg "$ps_" tag v1.0.2 HEAD
$GIT -C "$bare" tag v1.0.2 main~1
before=$($GIT -C "$bare" for-each-ref)
fails "push: an existing tag is never moved" in_mg "$ps_" push origin v1.0.2
same "push: the refused tag is unchanged there" "$before" "$($GIT -C "$bare" for-each-ref)"
in_mg "$ps_" branch old HEAD~1
before=$($GIT -C "$bare" rev-parse main)
case $(in_mg "$ps_" push origin old:main 2>&1) in *"[rejected]"*"old -> main (non-fast-forward)"*) ok "push: a non-fast-forward is refused" ;; *) bad "push of an older commit was not refused as a non-fast-forward" ;; esac
same "push: the refused branch is unchanged there" "$before" "$($GIT -C "$bare" rev-parse main)"
$GIT clone -q "$bare" "$tmp/other" 2>/dev/null
printf 'other side\n' > "$tmp/other/other.txt"
(cd "$tmp/other" && $GIT add -A && $GIT commit -q -m 'Other side' && $GIT push -q origin main) 2>/dev/null || bad "git push from the other clone"
printf 'mine\n' > "$ps_/mine.txt"
in_mg "$ps_" commit -q -m 'Mine' || bad "push fixture commit 3"
before=$($GIT -C "$bare" rev-parse main)
case $(in_mg "$ps_" push 2>&1) in *"fetch first"*) ok "push: a remote tip this side lacks is refused (fetch first)" ;; *) bad "push over an unknown remote tip was not refused as fetch first" ;; esac
same "push: the far side keeps the other clone's commit" "$before" "$($GIT -C "$bare" rev-parse main)"
in_mg "$ps_" branch side HEAD~1
in_mg "$ps_" checkout side >/dev/null 2>&1
printf 'side\n' > "$ps_/side.txt"
in_mg "$ps_" commit -q -m 'Side work' || bad "push fixture side commit"
$GIT -C "$bare" config receive.unpackLimit 1 # the far side keeps the pack (index-pack) instead of unpacking it
fails "push: a branch other than main is refused by the shipped rules" in_mg "$ps_" push origin side
same "push: the refused branch was not created" absent "$($GIT -C "$bare" rev-parse -q --verify side || echo absent)"
printf 'branch ^(main|side)$\n' >> "$XDG_CONFIG_HOME/mgit/check.conf"
in_mg "$ps_" push origin side >/dev/null 2>&1 || bad "push of side once the rules allow it"
same "push: the rules are read again on every run" "$(in_mg "$ps_" rev-parse side)" "$($GIT -C "$bare" rev-parse -q --verify side)"
same "push: the far side kept the pack (index-pack)" 1 "$(ls "$bare"/objects/pack/*.pack 2>/dev/null | wc -l)"
$GIT clone -q "$bare" "$tmp/back" 2>/dev/null && $GIT -C "$tmp/back" checkout -q side 2>/dev/null
same "push: git clone of the far side: the side branch" "$(in_mg "$ps_" rev-parse side)" "$(in_git "$tmp/back" rev-parse HEAD)"
if diff -r --exclude=.git "$ps_" "$tmp/back" >/dev/null; then ok "push: the clone's files are this side's"; else bad "push: the cloned files differ"; fi
printf 'token = "%s"\n' abcdef0123456789 > "$ps_/settings.ini"
in_mg "$ps_" commit -q -m 'Settings' || bad "push fixture secret commit"
before=$($GIT -C "$bare" for-each-ref)
out=$(in_mg "$ps_" push origin side 2>&1)
st=$?
same "push: the check refuses a secret" "1 refused" "$([ $st -ne 0 ] && echo 1) $(case $out in *"FAIL secret"*settings.ini:1*"nothing was sent"*) echo refused ;; esac)"
same "push: nothing reached the far side" "$before" "$($GIT -C "$bare" for-each-ref)"
case $(in_mg "$ps_" push https://host.example/x.git side 2>&1; echo " exit $?") in *"mgit pushes over ssh or to a path"*" exit 128") ok "push: an https url is refused" ;; *) bad "push to an https url was not refused as such" ;; esac
case $(in_mg "$ps_" push github.com:someone/x.git side 2>&1; echo " exit $?") in *"test ssh: refusing host github.com"*" exit 128") ok "push: the harness refuses any host but the stand-in" ;; *) bad "the harness let a push through to another host" ;; esac
in_mg "$ps_" remote add here "$tmp/plain.git"
$GIT init -q --bare "$tmp/plain.git"
in_mg "$ps_" push here main >/dev/null 2>&1 || bad "push to a remote whose url is a local path"
same "push: a remote with a local path takes the local push" "$(in_mg "$ps_" rev-parse main)" "$($GIT -C "$tmp/plain.git" rev-parse -q --verify main)"

# 28. the pre-push check alone: a clean repository passes, each rule fails on one planted hit
ck=$tmp/ck
mkdir -p "$ck"
sed -e 's/^warn-size .*/warn-size 2000/' -e 's/^max-size .*/max-size 5000/' "$testconf" > "$XDG_CONFIG_HOME/mgit/check.conf"
mg init "$ck/clean" >/dev/null
printf 'int main(void) { return 0; }\n' > "$ck/clean/main.c"
printf '*.o\n' > "$ck/clean/.gitignore"
in_mg "$ck/clean" commit -q -m 'Add main' -m 'A body.'
in_mg "$ck/clean" tag -a -m 'First release' v1.0.0
out=$(in_mg "$ck/clean" check 2>&1)
st=$?
same "check: a clean repository passes" "0 PASS" "$st $(printf '%s\n' "$out" | sed -n 's/^check: \(PASS\) .*/\1/p')"
# plant NAME RULE WHERE CMDS: a copy of the clean repository with CMDS run in it must fail with RULE at WHERE
plant() {
	label=$1 rule=$2 where=$3
	rm -rf "$ck/p"
	cp -a "$ck/clean" "$ck/p"
	(cd "$ck/p" && eval "$4") >/dev/null 2>&1
	out=$(in_mg "$ck/p" check 2>&1)
	st=$?
	hit=$(printf '%s\n' "$out" | grep -F "$rule" | grep -cF "$where")
	if [ $st -eq 1 ] && [ "$hit" -ge 1 ] && [ "$(printf '%s\n' "$out" | grep -c '^FAIL')" -eq "$hit" ]; then ok "check fails: $label"; else bad "check: $label ($st): $(printf '%s\n' "$out" | head -5)"; fi
}
c='mgit_commit() { "$MGIT" commit -q "$@"; }'
plant "an author with no email" identity "has no email" "$c; printf x > f; mgit_commit --author 'Cy <>' -m Subject"
plant "a second message line that is not blank" message "line 2 is not blank" "$c; printf x > f; mgit_commit -m \"\$(printf 'Subject\nno blank')\""
plant "a committed file a .gitignore ignores" ignored "debug.log:" "$c; printf x > debug.log; mgit_commit -m Log; printf '*.log\n' >> .gitignore; mgit_commit -m Ignore"
plant "a file over max-size" "FAIL size" "big.bin:" "$c; head -c 6000 /dev/zero > big.bin; mgit_commit -m Big"
plant "a private key" "secret:" "id_ed25519:1:" "$c; printf -- '-----BEGIN OPENSSH PRIVATE %s-----\nAAAA\n' KEY > id_ed25519; mgit_commit -m Key"
plant "an AWS key id" "secret:" "aws.txt:1:" "$c; printf 'id AKIA%s\n' IOSFODNN7EXAMPLE > aws.txt; mgit_commit -m Aws"
plant "a GitHub token" "secret:" "gh.txt:1:" "$c; printf 'ghp_%s\n' 0123456789abcdefghijklmnopqrstuvwxyzAB > gh.txt; mgit_commit -m Token"
plant "a password in code" "secret:" "conf.ini:2:" "$c; printf '[db]\npassword = \"hunter2hunter2\"\n' > conf.ini; mgit_commit -m Conf"
plant "a key in capitals, under (?i)" "secret:" "k.env:1:" "$c; printf 'API_KEY: \"abcdefgh123\"\n' > k.env; mgit_commit -m Key"
plant "a password in a url" "secret:" "u.txt:1:" "$c; printf 'https://bob:s3cretpw%shost.example/x\n' @ > u.txt; mgit_commit -m Url"
plant "an absolute home path" "private:" "n.txt:1:" "$c; printf 'see /Users/someone/x\n' > n.txt; mgit_commit -m Path"
plant "a path under the home directory, written with a tilde" "private:" "n.txt:1:" "$c; printf 'cd %s/scratch\n' '~' > n.txt; mgit_commit -m Path"
plant "a LAN address" "private:" "n.txt:1:" "$c; printf 'at 10.1.2.3\n' > n.txt; mgit_commit -m Lan"
plant "a host name" "private:" "n.txt:1:" "$c; printf 'built on hostA\n' > n.txt; mgit_commit -m Host"
plant "a session name" "private:" "n.txt:1:" "$c; printf 'ask session-a\n' > n.txt; mgit_commit -m Session"
plant "a private store's name" "private:" "n.txt:1:" "$c; printf 'never read private-store\n' > n.txt; mgit_commit -m Store"
plant "an email address" "private:" "n.txt:1:" "$c; printf 'mail bob%smail.example.net\n' @ > n.txt; mgit_commit -m Mail"
plant "an email address in the author" "private:" "commit" "$c; printf x > f; mgit_commit --author 'Bob <bob'@'mail.example.net>' -m Mail"
plant "a host name in a commit message" "private:" "message:3:" "$c; printf x > f; mgit_commit -m Subject -m 'Tested on hostB.'"
plant "a host name in a file name" "private:" "hostA-notes.txt:" "$c; printf x > hostA-notes.txt; mgit_commit -m Name"
plant "CR LF line endings" crlf "win.txt:1:" "$c; printf 'a\r\nb\r\n' > win.txt; mgit_commit -m Crlf"
plant "a branch other than main" branch "refs/heads/feature" "$c; \"\$MGIT\" branch feature; \"\$MGIT\" checkout feature"
plant "a tag that is not vX.Y.Z" "FAIL tag" "refs/tags/release-1" "$c; \"\$MGIT\" tag release-1"
plant "a tagger with no name" identity "has no name" "id=\$(printf 'object %s\\ntype commit\\ntag v2.0.0\\ntagger <t@example.com> 1 +0000\\n\\nRelease\\n' \$(git rev-parse HEAD) | git hash-object -t tag -w --literally --stdin) && git update-ref refs/tags/v2.0.0 \$id"
rm -rf "$ck/p"
cp -a "$ck/clean" "$ck/p"
head -c 3000 /dev/zero > "$ck/p/warn.bin"
in_mg "$ck/p" commit -q -m Warn
out=$(in_mg "$ck/p" check 2>&1)
same "check: a file over warn-size warns and passes" "0 1" "$? $(printf '%s\n' "$out" | grep -c '^WARN size *warn.bin:')"
printf 'see /Users/someone/x\n' > "$ck/p/n.txt"
in_mg "$ck/p" commit -q -m Path
fails "check: the planted path fails before it is allowed" in_mg "$ck/p" check
printf 'allow ^n\\.txt:1: /Users/$\n' >> "$XDG_CONFIG_HOME/mgit/check.conf"
quiet_ok=$(in_mg "$ck/p" check >/dev/null 2>&1; echo $?)
same "check: an allow rule takes the hit away, read on the next run" 0 "$quiet_ok"
printf 'mail x%sy.example.org at /Users/z\n' @ > "$ck/p/m.txt"
in_mg "$ck/p" commit -q -m Two
printf 'allow (?i)@y\\.example\\.org$\n' >> "$XDG_CONFIG_HOME/mgit/check.conf"
same "check: an allow rule covers only the match it names" "1 1" "$(in_mg "$ck/p" check > "$tmp/ck.out" 2>&1; echo $?) $(grep -c '^FAIL private:[0-9]* *m.txt:1:' "$tmp/ck.out")"
printf 'secret ([unclosed\n' >> "$XDG_CONFIG_HOME/mgit/check.conf"
fails "check: a broken pattern fails the check" in_mg "$ck/clean" check
{ cat "$testconf"; printf 'colour blue\n'; } > "$XDG_CONFIG_HOME/mgit/check.conf"
fails "check: an unknown rule fails the check" in_mg "$ck/clean" check
rm -f "$XDG_CONFIG_HOME/mgit/check.conf"
out=$(in_mg "$ck/clean" check 2>&1)
if [ -f /usr/local/share/mgit/check.conf ]; then
	case $out in *"rules /usr/local/share/mgit/check.conf"*) ok "check: with no settings file of its own, the shipped one" ;; *) bad "check without settings: $out" ;; esac
else
	case $out in *"no settings file"*) ok "check: with no settings file of its own, the shipped one" ;; *) bad "check without settings: $out" ;; esac
fi
# ---- v6: clone, fetch and pull ----
# 29. clone from a path, over ssh (git-upload-pack through the harness's ssh) and over https (the harness's curl,
# which serves https://standin/PATH through git-upload-pack --stateless-rpc and refuses every other host), each held
# against git clone of the same repository: refs, HEAD, log, the index, files, modes, symlinks; git fsck on each.
cat > "$fb/git-upload-pack" <<'EOF'
#!/bin/sh
# git's upload-pack, with what the client sends it appended to upload-in
tee -a "${0%/*}/upload-in" | /usr/bin/git-upload-pack "$@"
EOF
cat > "$fb/curl" <<'EOF'
#!/bin/sh
# test curl: serves https://standin/PATH from the repository at PATH through git-upload-pack (smart HTTP, protocol v0),
# answers four other hosts as a private, a dumb, a moved and a broken server would, and refuses every other host
printf '%s\n' "$*" >> "${0%/*}/curl-args"
url= post=0
for a; do case $a in http://* | https://*) url=$a ;; --data-binary) post=1 ;; esac; done
case $url in
https://standin/*) ;;
https://private/*) printf 'HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm="x"\r\nContent-Type: text/plain\r\n\r\nno\n'; exit 0 ;;
https://dumb/*) printf 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n0123456789abcdef0123456789abcdef01234567\trefs/heads/main\n'; exit 0 ;;
https://moved/*) printf 'HTTP/1.1 301 Moved Permanently\r\nLocation: https://standin/elsewhere.git/info/refs?service=git-upload-pack\r\nContent-Length: 0\r\n\r\n'; exit 0 ;;
https://broken/*) exit 7 ;;
*) echo "test curl: refusing $url" >&2; exit 6 ;;
esac
path=${url#https://standin}
case $path in
*/info/refs\?service=git-upload-pack)
	printf 'HTTP/1.1 200 OK\r\nContent-Type: application/x-git-upload-pack-advertisement\r\n\r\n001e# service=git-upload-pack\n0000'
	exec "${0%/*}/git-upload-pack" --stateless-rpc --http-backend-info-refs "${path%/info/refs*}" ;;
*/git-upload-pack)
	[ $post = 1 ] || exit 2
	printf 'HTTP/1.1 200 OK\r\nContent-Type: application/x-git-upload-pack-result\r\n\r\n'
	exec "${0%/*}/git-upload-pack" --stateless-rpc "${path%/git-upload-pack}" ;;
esac
exit 2
EOF
# the harness's ssh again, with one more host: "canned" answers with the file canned, whatever it is sent
cat > "$fb/ssh" <<'EOF'
#!/bin/sh
[ "$1" = -p ] && shift 2
case $1 in
standin | someone@standin) ;;
canned) cat "${0%/*}/canned"; exec cat > /dev/null ;;
*) echo "test ssh: refusing host $1" >&2; exit 255 ;;
esac
printf '%s\n' "$2" >> "${0%/*}/commands"
exec sh -c "$2"
EOF
chmod 755 "$fb/git-upload-pack" "$fb/curl" "$fb/ssh"
: > "$fb/upload-in"
: > "$fb/curl-args"
at() { export GIT_AUTHOR_DATE="$1 +0000" GIT_COMMITTER_DATE="$1 +0000"; }
cs=$tmp/clonesrc cb=$tmp/clone-bare.git gref=$tmp/clone-git
$GIT init -q -b main "$cs"
(
	cd "$cs" || exit 1
	n=0
	at 1800000000
	seq 1 4000 > data.txt
	printf '#!/bin/sh\necho run\n' > run.sh
	chmod 755 run.sh
	ln -s data.txt link
	mkdir -p dir/nested
	printf 'deep\n' > dir/nested/file
	printf 'version https://git-lfs.github.com/spec/v1\noid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\nsize 12345\n' > big.bin
	printf '*.bin filter=lfs diff=lfs merge=lfs -text\n*.bat eol=crlf\n' > .gitattributes
	printf '[submodule "sub"]\n\tpath = sub\n\turl = https://host.example/sub.git\n' > .gitmodules
	mkdir sub # where the submodule would be cloned; git takes a missing directory for a deleted submodule
	$GIT add -A && $GIT update-index --add --cacheinfo "160000,$($GIT -C "$m" rev-parse HEAD),sub" && $GIT commit -q -m Start
	while [ $n -lt 80 ]; do
		n=$((n + 1))
		at $((1800000000 + 60 * n))
		seq "$n" 4000 >> data.txt
		$GIT commit -q -a -m "Step $n"
	done
	$GIT tag -a -m 'Version 1.0.0' v1.0.0 HEAD~3
	$GIT tag light HEAD~5
	$GIT checkout -q -b side HEAD~10
	at 1800006000
	printf 'side\n' > side.txt
	$GIT add -A && $GIT commit -q -m Side
	$GIT checkout -q main
	at 1800006100
	$GIT merge -q --no-edit side
) || bad "clone source fixture"
$GIT clone -q --bare "$cs" "$cb" 2>/dev/null
$GIT -C "$cb" gc -q --aggressive
same "clone source: the bare copy is one pack with deltas" "1 yes" "$(ls "$cb"/objects/pack/*.pack | wc -l) $($GIT verify-pack -v "$cb"/objects/pack/*.idx | grep -q '^chain length' && echo yes)"
$GIT clone -q "$cb" "$gref" 2>/dev/null
listing() { (cd "$1" && find . -path ./.git -prune -o -printf '%y %m %p %l\n' | sort && find . -path ./.git -prune -o -type f -print | sort | xargs sha256sum); }
refs_of() { $GIT -C "$1" for-each-ref --format='%(objectname) %(refname)'; }
clone_cmp() {
	label=$1 d=$2
	quiet "clone $label: git fsck --strict on the clone" $GIT -C "$d" fsck --strict --no-dangling
	same "clone $label: refs as git clone's" "$(refs_of "$gref")" "$(refs_of "$d")"
	same "clone $label: HEAD as git clone's" "$($GIT -C "$gref" symbolic-ref HEAD) $($GIT -C "$gref" rev-parse HEAD)" "$($GIT -C "$d" symbolic-ref HEAD) $($GIT -C "$d" rev-parse HEAD)"
	same "clone $label: origin/HEAD as git clone's" "$($GIT -C "$gref" symbolic-ref refs/remotes/origin/HEAD)" "$($GIT -C "$d" symbolic-ref refs/remotes/origin/HEAD 2>&1)"
	same "clone $label: the whole history as git clone's" "$($GIT -C "$gref" log --all --format='%H %P %T %s')" "$($GIT -C "$d" log --all --format='%H %P %T %s')"
	same "clone $label: mgit log as git log --first-parent" "$(in_git "$gref" log --first-parent)" "$(in_mg "$d" log)"
	same "clone $label: files, modes and symlinks as git clone's" "$(listing "$gref")" "$(listing "$d")"
	same "clone $label: the index as git clone's" "$($GIT -C "$gref" ls-files -s)" "$($GIT -C "$d" ls-files -s)"
	same "clone $label: git status clean" "clean" "$(out=$($GIT -C "$d" status --porcelain) && [ -z "$out" ] && echo clean)"
	same "clone $label: mgit status clean" "$clean" "$(in_mg "$d" status | tail -n 1)"
	same "clone $label: remote origin and branch main's upstream" "$3 origin refs/heads/main" "$($GIT -C "$d" config remote.origin.url) $($GIT -C "$d" config branch.main.remote) $($GIT -C "$d" config branch.main.merge)"
}
cout=$(cd "$tmp" && mg clone "$cb" clone-path 2>&1)
same "clone path: exit 0" 0 $?
clone_cmp path "$tmp/clone-path" "$(cd "$cb" && pwd -P)"
case $cout in *"notice: 1 submodule not fetched"*": sub"*) ok "clone: a notice names the submodule not fetched" ;; *) bad "clone submodule notice: $cout" ;; esac
case $cout in *"notice: 1 file is a Git LFS pointer, left as it is"*": big.bin"*) ok "clone: a notice names the LFS pointer left as it is" ;; *) bad "clone LFS notice: $cout" ;; esac
case $cout in *"notice: .gitattributes asks for filter=lfs (.gitattributes:1), eol=crlf (.gitattributes:2)"*) ok "clone: a notice names the filter and eol attributes not applied" ;; *) bad "clone attributes notice: $cout" ;; esac
mkdir -p "$tmp/guess" && (cd "$tmp/guess" && mg clone "standin:$cb/" >/dev/null 2>&1 && mg clone "https://standin$cb" x/y/ >/dev/null 2>&1)
same "clone: the directory is named as git names it, and made with its parents" "clone-bare $($GIT -C "$cb" rev-parse main)" "$(ls "$tmp/guess" | head -n 1) $(in_git "$tmp/guess/x/y" rev-parse HEAD)"
mkdir "$tmp/clone-ssh"
mg clone "standin:$cb" "$tmp/clone-ssh" > /dev/null 2>&1
same "clone ssh: exit 0 (into an empty directory that was there)" 0 $?
grep -qxF "git-upload-pack '$cb'" "$fb/commands" && ok "clone ssh: runs git-upload-pack 'PATH' on the far side" || bad "clone ssh ran: $(tail -n 2 "$fb/commands")"
clone_cmp ssh "$tmp/clone-ssh" "standin:$cb"
mg clone "https://standin$cb" "$tmp/clone-https" > /dev/null 2>&1
same "clone https: exit 0" 0 $?
clone_cmp https "$tmp/clone-https" "https://standin$cb"
case $(head -n 1 "$fb/curl-args") in "-q --silent --show-error "*" --user-agent mgit/"*" https://standin$cb/info/refs?service=git-upload-pack") ok "clone https: curl runs with -q (no .curlrc) and no credentials" ;; *) bad "curl was run as: $(head -n 1 "$fb/curl-args")" ;; esac
grep -c -e ' -u ' -e ' --user ' -e '--netrc' -e ' -n ' -e 'Authorization' -e '--oauth2' -e ' -K ' -e '--config ' "$fb/curl-args" > "$tmp/credhits"
same "clone https: no credential option reaches curl (in each of its runs)" "yes 0" "$([ -s "$fb/curl-args" ] && echo yes) $(cat "$tmp/credhits")"
same "clone ssh: the pack came in deltas and stays one pack" "1 yes" "$(ls "$tmp"/clone-ssh/.git/objects/pack/*.pack | wc -l) $($GIT verify-pack -v "$tmp"/clone-ssh/.git/objects/pack/*.idx | grep -q '^chain length' && echo yes)"
$GIT -C "$tmp/clone-ssh" verify-pack "$tmp"/clone-ssh/.git/objects/pack/*.idx >/dev/null 2>&1 && ok "clone ssh: git verify-pack accepts the idx mgit wrote" || bad "git verify-pack refused the clone's idx"
cp "$(ls "$tmp"/clone-ssh/.git/objects/pack/*.pack)" "$tmp/re.pack"
$GIT index-pack -o "$tmp/re.idx" "$tmp/re.pack" >/dev/null 2>&1
same_files "clone ssh: the idx is byte for byte the one git index-pack writes for the same pack" "$tmp/re.idx" "$(ls "$tmp"/clone-ssh/.git/objects/pack/*.idx)"
cp "$(ls "$tmp"/clone-https/.git/objects/pack/*.pack)" "$tmp/re.pack"
$GIT index-pack -o "$tmp/re.idx" "$tmp/re.pack" >/dev/null 2>&1
same_files "clone https: the idx is byte for byte the one git index-pack writes for the same pack" "$tmp/re.idx" "$(ls "$tmp"/clone-https/.git/objects/pack/*.idx)"

# 30. fetch: new commits, a new branch, a force-moved branch and a new tag, against git fetch --tags in git's clone
(
	cd "$cs" || exit 1
	at 1800010000
	printf 'more\n' >> data.txt
	$GIT commit -q -a -m 'More'
	at 1800010100
	printf 'feature\n' > feature.txt
	$GIT checkout -q -b feature
	$GIT add -A && $GIT commit -q -m Feature
	$GIT checkout -q side
	$GIT reset -q --hard HEAD~1
	at 1800010200
	printf 'side again\n' > side.txt
	$GIT add -A && $GIT commit -q -m 'Side again'
	$GIT checkout -q main
	$GIT tag -a -m 'Version 1.1.0' v1.1.0
	$GIT -C "$cb" fetch -q --force "$cs" +main:main +feature:feature +side:side refs/tags/v1.1.0:refs/tags/v1.1.0
) 2>/dev/null || bad "fetch fixture"
$GIT -C "$gref" fetch -q --tags origin 2>/dev/null
for t in path ssh https; do
	d=$tmp/clone-$t
	refs_of "$d" | cut -d' ' -f1 > "$tmp/old-$t"
	ls "$d"/.git/objects/pack/*.idx > "$tmp/packs-$t"
	fout=$(in_mg "$d" fetch 2>&1)
	same "fetch $t: exit 0" 0 $?
	same "fetch $t: refs as git fetch --tags's" "$(refs_of "$gref")" "$(refs_of "$d")"
	quiet "fetch $t: git fsck --strict" $GIT -C "$d" fsck --strict --no-dangling
	new=$(ls "$d"/.git/objects/pack/*.idx | grep -vxF -f "$tmp/packs-$t")
	want=$($GIT -C "$cb" rev-list --objects --all --not $(cat "$tmp/old-$t") | wc -l)
	same "fetch $t: one new pack with only what the clone lacked ($want objects)" "1 $want" "$(printf '%s\n' "$new" | grep -c .) $($GIT show-index < "$new" | wc -l)"
	case $fout in *"[new branch]"*"feature -> origin/feature"*) ok "fetch $t: reports the new branch" ;; *) bad "fetch $t output: $fout" ;; esac
	case $fout in *"+ "*"side -> origin/side  (forced update)"*) ok "fetch $t: a forced update of a tracking ref, as the + in its fetch line allows" ;; *) bad "fetch $t forced: $fout" ;; esac
	case $fout in *"[new tag]"*"v1.1.0 -> v1.1.0"*) ok "fetch $t: reports the new tag" ;; *) bad "fetch $t tag: $fout" ;; esac
	out=$(in_mg "$d" fetch 2>&1)
	same "fetch $t: a second fetch finds nothing and says nothing" "0 [] 1" "$? [$out] $(ls "$d"/.git/objects/pack/*.idx | grep -vxF -f "$tmp/packs-$t" | grep -c .)"
done
(cd "$cs" && $GIT tag -f light HEAD~2 && $GIT -C "$cb" fetch -q --force "$cs" +refs/tags/light:refs/tags/light) >/dev/null 2>&1
before=$(refs_of "$tmp/clone-ssh")
out=$(in_mg "$tmp/clone-ssh" fetch 2>&1)
st=$?
case "$st $out" in "1 "*"! [rejected]"*"light -> light  (would clobber existing tag)"*) ok "fetch: a tag that moved there is refused here (would clobber), exit 1" ;; *) bad "fetch tag clobber ($st): $out" ;; esac
same "fetch: the refused tag is unchanged here" "$before" "$(refs_of "$tmp/clone-ssh")"
$GIT -C "$tmp/clone-ssh" tag -d light >/dev/null
ls "$tmp"/clone-ssh/.git/objects/pack/*.idx > "$tmp/packs-ssh"
: > "$fb/upload-in"
fout=$(in_mg "$tmp/clone-ssh" fetch 2>&1)
st=$?
same "fetch: a tag at a commit here already: the ref moves, no want is sent, no pack comes" "0 $($GIT -C "$cb" rev-parse light) 0 0" "$st $(in_git "$tmp/clone-ssh" rev-parse light) $(grep -c 'want ' "$fb/upload-in") $(ls "$tmp"/clone-ssh/.git/objects/pack/*.idx | grep -vxF -f "$tmp/packs-ssh" | grep -c .)"
$GIT -C "$tmp/clone-path" config remote.origin.fetch 'refs/heads/*:refs/remotes/origin/*'
(cd "$cs" && $GIT checkout -q side && $GIT reset -q --hard HEAD~1 && at 1800010300 && printf 'side 3\n' > side.txt && $GIT add -A && $GIT commit -q -m 'Side 3' && $GIT checkout -q main && $GIT -C "$cb" fetch -q --force "$cs" +side:side) >/dev/null 2>&1
before=$(in_git "$tmp/clone-path" rev-parse origin/side)
out=$(in_mg "$tmp/clone-path" fetch 2>&1)
st=$?
case "$st $out" in "1 "*"! [rejected]"*"side -> origin/side  (non-fast-forward)"*) ok "fetch: without + in the fetch line a non-fast-forward is refused" ;; *) bad "fetch non-ff ($st): $out" ;; esac
same "fetch: the refused tracking ref is unchanged" "$before" "$(in_git "$tmp/clone-path" rev-parse origin/side)"
same "fetch: an unknown remote is refused" "128 1" "$(in_mg "$tmp/clone-path" fetch nosuch > "$tmp/out" 2>&1; echo $?) $(grep -c 'no remote named nosuch' "$tmp/out")"

# 31. negotiation: 40 commits of local work newer than anything the remote has, then a fetch of 2 new commits. The haves
# go newest first in rounds of 32; the second round meets commits the remote has, so 64 haves are sent, not all 122.
ng=$tmp/clone-neg
mg clone "standin:$cb" "$ng" > /dev/null 2>&1 || bad "negotiation clone"
(
	cd "$ng" || exit 1
	"$MGIT" branch local && "$MGIT" checkout local >/dev/null || exit 1
	n=0
	while [ $n -lt 40 ]; do
		n=$((n + 1))
		at $((1800100000 + n))
		printf '%s\n' "$n" >> local.txt
		"$MGIT" commit -q -m "Local $n" || exit 1
	done
	"$MGIT" checkout main >/dev/null
) || bad "negotiation fixture"
(cd "$cs" && for k in 1 2; do at $((1800200000 + k)) && printf 'new %s\n' "$k" >> data.txt && $GIT commit -q -a -m "New $k"; done && $GIT -C "$cb" fetch -q "$cs" main:main) >/dev/null 2>&1
old=$(refs_of "$ng" | cut -d' ' -f1)
ls "$ng"/.git/objects/pack/*.idx > "$tmp/packs-neg"
: > "$fb/upload-in"
in_mg "$ng" fetch > /dev/null 2>&1 || bad "negotiation fetch"
same "fetch: haves stop with the round that meets a common commit (64 sent, not every commit here)" 64 "$(grep -ac 'have [0-9a-f]\{40\}' "$fb/upload-in")"
new=$(ls "$ng"/.git/objects/pack/*.idx | grep -vxF -f "$tmp/packs-neg")
same "fetch: after the negotiation the pack holds only the 2 new commits' objects" "6 6" "$($GIT -C "$ng" rev-list --objects origin/main --not $old | wc -l) $($GIT show-index < "$new" | wc -l)"
same "fetch: the new commits arrived" "$($GIT -C "$cb" rev-parse main)" "$(in_git "$ng" rev-parse origin/main)"
mg clone "https://standin$cb" "$tmp/neg-https" > /dev/null 2>&1 || bad "negotiation https clone"
(cd "$tmp/neg-https" && "$MGIT" branch local && "$MGIT" checkout local >/dev/null && n=0 && while [ $n -lt 40 ]; do n=$((n + 1)); at $((1800300000 + n)); printf '%s\n' "$n" >> local.txt; "$MGIT" commit -q -m "Local $n"; done && "$MGIT" checkout main >/dev/null) || bad "https negotiation fixture"
(cd "$cs" && at 1800400000 && printf 'newer\n' >> data.txt && $GIT commit -q -a -m Newer && $GIT -C "$cb" fetch -q "$cs" main:main) >/dev/null 2>&1
old=$(refs_of "$tmp/neg-https" | cut -d' ' -f1)
ls "$tmp"/neg-https/.git/objects/pack/*.idx > "$tmp/packs-neg"
: > "$fb/curl-args"
in_mg "$tmp/neg-https" fetch > /dev/null 2>&1 || bad "https negotiation fetch"
same "fetch https: two rounds of haves, then the request with done (3 POSTs)" 3 "$(grep -c -e '--data-binary' "$fb/curl-args")"
new=$(ls "$tmp"/neg-https/.git/objects/pack/*.idx | grep -vxF -f "$tmp/packs-neg")
same "fetch https: the pack holds only the new commit's objects" "3 3" "$($GIT -C "$tmp/neg-https" rev-list --objects origin/main --not $old | wc -l) $($GIT show-index < "$new" | wc -l)"

# 32. pull: fetch, then fast-forward only
c=$tmp/clone-ssh
out=$(in_mg "$c" pull 2>&1)
same "pull: fast-forwards the branch to origin's" "$($GIT -C "$cb" rev-parse main)" "$(in_git "$c" rev-parse HEAD)"
case $out in *"Fast-forward"*) ok "pull: says Fast-forward" ;; *) bad "pull output: $out" ;; esac
same "pull: the files are the remote's" "$($GIT -C "$cb" show main:data.txt | sha256sum)" "$(sha256sum < "$c/data.txt")"
same "pull: git status clean after it" "clean" "$(out=$($GIT -C "$c" status --porcelain) && [ -z "$out" ] && echo clean)"
same "pull: up to date the second time" "Already up to date." "$(in_mg "$c" pull 2>/dev/null | tail -n 1)"
printf 'mine\n' > "$c/mine.txt"
in_mg "$c" commit -q -m 'Mine' || bad "pull fixture commit"
(cd "$cs" && at 1800500000 && printf 'theirs\n' >> data.txt && $GIT commit -q -a -m Theirs && $GIT -C "$cb" fetch -q "$cs" main:main) >/dev/null 2>&1
before=$(in_git "$c" rev-parse HEAD)
out=$(in_mg "$c" pull 2>&1)
st=$?
case "$st $out" in "128 "*"not a fast-forward"*) ok "pull: refuses diverged history (no merge)" ;; *) bad "pull diverged ($st): $out" ;; esac
same "pull: the refused pull leaves HEAD and the files" "$before mine" "$(in_git "$c" rev-parse HEAD) $(cat "$c/mine.txt")"
same "pull: the refused pull still fetched" "$($GIT -C "$cb" rev-parse main)" "$(in_git "$c" rev-parse origin/main)"
out=$(in_mg "$tmp/clone-https" pull 2>&1)
same "pull https: fast-forwards" "$($GIT -C "$cb" rev-parse main)" "$(in_git "$tmp/clone-https" rev-parse HEAD)"
in_mg "$tmp/clone-https" checkout "$(in_git "$tmp/clone-https" rev-parse HEAD)" >/dev/null 2>&1
same "pull: refused on a detached HEAD" "128 1" "$(in_mg "$tmp/clone-https" pull > "$tmp/out" 2>&1; echo $?) $(grep -c 'HEAD is detached' "$tmp/out")"
$GIT init -q --bare -b main "$tmp/empty.git"
out=$(cd "$tmp" && mg clone "$tmp/empty.git" empty-clone 2>&1)
same "clone: an empty repository: exit 0, a warning, HEAD on main" "0 1 refs/heads/main" "$? $(printf '%s\n' "$out" | grep -c 'empty repository') $($GIT -C "$tmp/empty-clone" symbolic-ref HEAD)"
$GIT -C "$tmp/empty.git" fetch -q "$cs" main:main >/dev/null 2>&1
in_mg "$tmp/empty-clone" pull >/dev/null 2>&1
same "pull: into the clone of an empty repository checks out its first branch" "$($GIT -C "$cs" rev-parse main) clean" "$(in_git "$tmp/empty-clone" rev-parse HEAD) $(out=$($GIT -C "$tmp/empty-clone" status --porcelain) && [ -z "$out" ] && echo clean)"

# 33. refusals: SHA-256 repositories, credentials, other schemes, servers that are not smart HTTP, an occupied
# directory; and packs that are wrong in each way a far side could send them (the canned host), none of which may
# leave a clone or move a ref.
s2w=$tmp/s256w s2=$tmp/s256.git
$GIT init -q --object-format=sha256 -b main "$s2w" && printf 'x\n' > "$s2w/x" && $GIT -C "$s2w" add -A && $GIT -C "$s2w" commit -q -m x
$GIT clone -q --bare "$s2w" "$s2" 2>/dev/null
for t in path ssh https; do
	case $t in path) u=$s2 ;; ssh) u=standin:$s2 ;; https) u=https://standin$s2 ;; esac
	out=$(cd "$tmp" && mg clone "$u" s256-clone 2>&1)
	st=$?
	same "clone $t refuses a SHA-256 repository, leaving nothing" "128 1 absent" "$st $(printf '%s\n' "$out" | grep -c 'sha256 object names; mgit works with SHA-1 repositories only') $([ -e "$tmp/s256-clone" ] || echo absent)"
done
lines=$(wc -l < "$fb/curl-args")
out=$(cd "$tmp" && mg clone https://bob:pw@standin/x.git cred-clone 2>&1)
same "clone refuses an https url with a user name or password, and runs no curl" "128 1 $lines absent" "$? $(printf '%s\n' "$out" | grep -c 'anonymously only') $(wc -l < "$fb/curl-args") $([ -e "$tmp/cred-clone" ] || echo absent)"
case $(cd "$tmp" && mg clone git://standin/x.git 2>&1; echo " exit $?") in *"not git://"*" exit 128") ok "clone refuses git:// (ssh, https or a path only)" ;; *) bad "git:// was not refused" ;; esac
case $(cd "$tmp" && mg clone https://private/x.git 2>&1; echo " exit $?") in *"HTTP 401"*"anonymously only"*" exit 128") ok "clone: a server asking for credentials (401) is refused as such" ;; *) bad "401 was not refused as such" ;; esac
case $(cd "$tmp" && mg clone https://dumb/x.git 2>&1; echo " exit $?") in *"not a smart git http server"*" exit 128") ok "clone: a dumb http server is refused" ;; *) bad "a dumb http server was not refused" ;; esac
case $(cd "$tmp" && mg clone https://moved/x.git 2>&1; echo " exit $?") in *"HTTP 301"*"moved to https://standin/elsewhere.git"*" exit 128") ok "clone: a redirect is named, not followed" ;; *) bad "a redirect was not refused as such" ;; esac
case $(cd "$tmp" && mg clone https://broken/x.git 2>&1; echo " exit $?") in *"curl ended badly (exit 7)"*" exit 128") ok "clone: curl's own failure is reported with its exit status" ;; *) bad "a failing curl was not reported" ;; esac
mkdir -p "$tmp/occupied" && printf 'keep\n' > "$tmp/occupied/keep"
out=$(cd "$tmp" && mg clone "$cb" occupied 2>&1)
same "clone refuses a directory that is not empty, and leaves it" "128 1 keep" "$? $(printf '%s\n' "$out" | grep -c 'already exists and is not an empty directory') $(ls "$tmp/occupied")"
case $(cd "$tmp" && mg clone github.com:someone/x.git 2>&1; echo " exit $?") in *"test ssh: refusing host github.com"*" exit 128") ok "clone: the harness refuses any ssh host but its own" ;; *) bad "the harness let a clone through to another host" ;; esac
# canned PACKFILE: the bare source's advertisement (as git-upload-pack gives it), NAK, then PACKFILE in side-band-64k
canned() {
	printf 0000 | /usr/bin/git-upload-pack "$cb" > "$fb/canned"
	printf '0008NAK\n' >> "$fb/canned"
	rm -rf "$tmp/sb" && mkdir "$tmp/sb" && split -b 65000 "$1" "$tmp/sb/x"
	for ch in "$tmp"/sb/x*; do printf '%04x\001' $(($(wc -c < "$ch") + 5)); cat "$ch"; done >> "$fb/canned"
	printf 0000 >> "$fb/canned"
}
refused() {
	label=$1 why=$2
	out=$(cd "$tmp" && mg clone canned:x canned-clone 2>&1)
	st=$?
	same "clone refuses $label, leaving nothing" "128 1 absent" "$st $(printf '%s\n' "$out" | grep -c "$why") $([ -e "$tmp/canned-clone" ] || echo absent)"
}
$GIT -C "$cb" pack-objects -q --revs --all --stdout --delta-base-offset < /dev/null > "$tmp/good.pack"
canned "$tmp/good.pack"
out=$(cd "$tmp" && mg clone canned:x canned-clone 2>&1)
st=$?
same "clone canned: the canned answer itself is good" "0 $($GIT -C "$cb" rev-parse main)" "$st $(in_git "$tmp/canned-clone" rev-parse HEAD)"
rm -rf "$tmp/canned-clone"
head -c -1 "$tmp/good.pack" > "$tmp/bad.pack"
if [ "$(tail -c 1 "$tmp/good.pack" | od -An -tx1 | tr -d ' ')" = 00 ]; then printf '\001'; else printf '\000'; fi >> "$tmp/bad.pack"
canned "$tmp/bad.pack"
refused "a pack whose SHA-1 trailer does not match" "SHA-1 trailer does not match"
head -c 2000 "$tmp/good.pack" > "$tmp/bad.pack"
canned "$tmp/bad.pack"
refused "a pack cut short" "the pack that came is refused"
$GIT -C "$cb" rev-list --all | $GIT -C "$cb" pack-objects -q --stdout > "$tmp/bad.pack"
canned "$tmp/bad.pack"
refused "a pack of commits without their trees" "lacks object"
$GIT -C "$m" pack-objects -q --revs --all --stdout < /dev/null > "$tmp/bad.pack"
canned "$tmp/bad.pack"
refused "a pack of some other repository's objects" "lacks object"
$GIT -C "$cb" pack-objects -q --revs --all --stdout --no-reuse-delta < /dev/null > "$tmp/ref.pack"
same "clone canned: the second pack has deltas (written without --delta-base-offset: REF_DELTA)" yes "$($GIT index-pack -o "$tmp/ref.idx" "$tmp/ref.pack" >/dev/null 2>&1 && $GIT verify-pack -v "$tmp/ref.idx" | grep -q '^chain length' && echo yes)"
canned "$tmp/ref.pack"
out=$(cd "$tmp" && mg clone canned:x canned-ref 2>&1)
st=$?
same "clone: a pack of REF_DELTA entries is indexed and checked" "0 $($GIT -C "$cb" rev-parse main)" "$st $(in_git "$tmp/canned-ref" rev-parse HEAD)"
quiet "clone: git fsck --strict on the REF_DELTA clone" $GIT -C "$tmp/canned-ref" fsck --strict --no-dangling
printf 'pass %d  fail %d\n' "$pass" "$fail"
exit $((fail > 0))
