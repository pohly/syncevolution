#! /bin/bash

set -o pipefail

# We cannot rely on cppcheck --exitcode because it gets triggered by
# suppressed errors. Instead look at the output.
cppcheck '--template={file}:{line}: cppcheck {severity}: {id} - {message}' "$@" 2>&1 | \
         perl -e '$res = 0; while (<>) { if (/: cppcheck /) { $res = 1; }; print; }; exit $res;'
