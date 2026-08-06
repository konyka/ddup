# Patches applied to vendored Lua 5.1.5

None. The sources in this directory are byte-identical to the official
lua-5.1.5.tar.gz distribution (https://www.lua.org/ftp/lua-5.1.5.tar.gz),
minus the interpreter/bytecode-dump main programs (lua.c, luac.c, print.c),
which are not needed for embedding.

If a future build issue ever requires modifying these sources, document
every change here: file, hunk, reason, and upstream ticket if any.
