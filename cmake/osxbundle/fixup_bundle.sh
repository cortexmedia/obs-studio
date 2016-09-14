#!/bin/bash

if [ "$#" != 2 ]; then
	echo "usage: $0 /path/to/install/root relative/lib/destination"
	exit 1
fi

cd "$1"

DEST="$2"
LDEST="@rpath"
QT_PLUGINS_DIR=/usr/local/Cellar/qt5/5.6.1-1/plugins

function copy_qt_plugins() {
	cp -R "$QT_PLUGINS_DIR/$1" "$DEST/"

	PLUGINS="$(find $DEST/$1 \( -perm +111 -and -type f \))"

	for lib in $PLUGINS; do
		echo "Fixing Qt plugin $lib"

		libname="$(basename "$lib")"
		install_name_tool -id "$LDEST/$1/$libname" "$lib"
	done
}

function buildlist() {
	otool -L "$@" | 
		grep -E '^\s+/' |
		grep -vE '^\s+/System/' |
		grep -vE '^\s+/usr/lib/' |
		perl -pe 's|^\s+(/.*)\s\(.*$|$1|' |
		grep -vE ":$" |
		sort -u
}
export -f buildlist

copy_qt_plugins platforms
copy_qt_plugins imageformats

TARGETS="$(find . \( -perm +111 -and -type f \))"
FOUNDLIBS="$(buildlist $TARGETS)"
PFOUNDLIBS=""

while [ "$FOUNDLIBS" != "$PFOUNDLIBS" ]; do
	PFOUNDLIBS="$FOUNDLIBS"
	FOUNDLIBS="$(buildlist $TARGETS $PFOUNDLIBS)"
done

INTOOL_CALL=()

for lib in $FOUNDLIBS; do
	libname="$(basename "$lib")"

	INTOOL_CALL+=(-change "$lib" "$LDEST/$libname")
	cp "$lib" "$DEST/$libname"
	chmod 644 "$DEST/$libname"

	echo "Fixing up dependency: $libname"
done

for lib in $FOUNDLIBS; do
	libname="$(basename "$lib")"
	lib="$DEST/$libname"

	install_name_tool ${INTOOL_CALL[@]} -id "$LDEST/$libname" "$lib"
done

for target in $TARGETS; do
	install_name_tool ${INTOOL_CALL[@]} "$target"
done

