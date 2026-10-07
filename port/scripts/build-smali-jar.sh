#!/bin/bash
# build-smali-jar.sh - build a fat executable smali.jar from Maven artifacts.
#
# The Maven Central `org.smali:smali` jar has `org.jf.smali.Main` but no
# `Main-Class` manifest, and the CLI needs its dependencies (jcommander, guava,
# antlr, dexlib2, util, stringtemplate).  This script downloads the artifacts
# and repackages them into a single executable jar at tools/smali.jar.
#
# Usage:  bash port/scripts/build-smali-jar.sh
#
# After this, mkinitramfs.sh can build porthelper.jar (the keep-awake helper).
# Without it the initramfs still builds, but the screen will time out on device.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$REPO/tools"
M2=https://repo1.maven.org/maven2

say() { printf '\n==> %s\n' "$*"; }

say "downloading smali + dependencies from Maven Central"
SB="$(mktemp -d)"
cd "$SB"

# smali 2.5.2 and its dependencies (versions pinned to the smali 2.5.2 POM).
download() {
	local group_path="$1" artifact="$2" version="$3"
	local url="$M2/$group_path/$artifact/$version/$artifact-$version.jar"
	echo "    $artifact-$version"
	curl -fsSL -o "$artifact-$version.jar" "$url"
}

download org/smali smali 2.5.2
download org/smali util 2.5.2
download org/smali dexlib2 2.5.2
download org/smali antlr 2.5.2
download org/smali antlr-runtime 2.5.2
download org/smali stringtemplate 2.5.2
download com/google/guava guava 27.0.1-jre
download com/beust jcommander 1.72

say "repackaging into a fat executable jar"
# Extract all jars into one directory, then re-jar with a Main-Class manifest.
mkdir -p classes
cd classes
for j in ../*.jar; do
	unzip -qo "$j" -d . 2>/dev/null || true
done
# Remove signature files (they break the fat jar).
rm -f META-INF/*.SF META-INF/*.DSA META-INF/*.RSA 2>/dev/null || true

# Build the fat jar with a Main-Class manifest.
jar cfe "$TOOLS/smali.jar" org.jf.smali.Main .

cd /
rm -rf "$SB"

say "built $TOOLS/smali.jar"
echo "    size: $(stat -c%s "$TOOLS/smali.jar") bytes"
echo "    test: java -jar $TOOLS/smali.jar --version"
