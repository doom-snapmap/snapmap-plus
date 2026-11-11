# Lua 5.4.9 (vendored source)

The Lua interpreter core and the libraries mod scripts may use, compiled into
the backend DLL by `src/backend/build.ps1` and run only through
`src/backend/script/` (docs/architecture.md, "Mod scripts"). MIT licence:
`LICENSE` in this folder (the notice at the end of `lua.h`).

- Upstream: https://www.lua.org/ftp/lua-5.4.9.tar.gz, released 2026-08-10.
- Tarball SHA-256: `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`
  (the checksum lua.org lists for it, and the bytes these files came from).
- Taken from the tarball's `src/`: the core, the auxiliary library and the
  base, coroutine, string, table, math and utf8 libraries. Left out, so they
  are not in the product at all: `liolib.c`, `loslib.c`, `loadlib.c` (package),
  `ldblib.c` (debug library; `ldebug.c` is the core's), `linit.c`, `lua.c`,
  `luac.c`, `lua.hpp`, the Makefiles and `doc/`.
- Build: compiled as C with the toolchain defaults of `luaconf.h` (no
  `LUA_USE_APICHECK`; `LUAI_MAXCCALLS` 200, its default). `lstrlib.c` is
  compiled only through `src/backend/script/script_lstrlib.c`, which defines
  the hook below.

## Local change

One, in `lstrlib.c`: a hook `l_matchstep(L)` called once per pattern-matching
step, defaulting to nothing, so the embedder can bound the total work of one
pattern operation (C pattern matching never reaches the count hook, and a
backtracking pattern can run for minutes on a 4 KB subject):

```diff
@@ -379,6 +379,14 @@
 #endif
 
 
+/* Snapmap+ local change (README.md in this folder): called once per 'match'
+** step so an embedder can bound the total work of one pattern operation,
+** which the instruction count hook cannot interrupt. */
+#if !defined(l_matchstep)
+#define l_matchstep(L)	((void)0)
+#endif
+
+
 #define L_ESC		'%'
 #define SPECIALS	"^$*+?.([%-"
 
@@ -570,6 +578,7 @@
 static const char *match (MatchState *ms, const char *s, const char *p) {
   if (l_unlikely(ms->matchdepth-- == 0))
     luaL_error(ms->L, "pattern too complex");
+  l_matchstep(ms->L);
   init: /* using goto to optimize tail recursion */
   if (p != ms->p_end) {  /* end of pattern? */
     switch (*p) {
```

`lstrlib.c` upstream SHA-256: `d386f8bbb75d5ef7b04c8ca9e747ff256525a9546bb6ff01c788b912b6995aa6`;
with the change: `a4461a89e2bd444cabaf0b8cfb5b2380750ec4114426da8c822239e06264a875`.

## Files

SHA-256 of each upstream file (LF line endings, as in the tarball and as
committed); every file here but `lstrlib.c` is byte-identical to it.

```
cd369dc6900a7696ca55ccbd4f50eadfa799b975f34b7afe450e1b859517a56e  lapi.c
9522209670086adfbb11379416078a7f4dafe154043e320cd435fa1811aaabfb  lapi.h
257c287ed4964ec8bb07fe0a1fd1b52b712e4bccd6c889db62f84abb905d6996  lauxlib.c
2d9e18e577a6062646268e7f7228264c28bd9b972c186e1af81b8357179c0ab4  lauxlib.h
5dc9a64468c1ac31e59066c7d3597eeb8ea4ac8fc19a257a74c431624300b487  lbaselib.c
2cf13c7f8205e9f07c2ccf1fedc3f0f0a05d238c194b176c72cc22b2cc96210e  lcode.c
c5ad73fb12fd3e9f6b6a5f37fea5b60e4c09f8a262e9ac3ca85e8e047f745cea  lcode.h
5408ccec737678d045d421f84a56e1ce03192a42c55440ffef07bec356aaec89  lcorolib.c
3e21ae6a8faab3ed470ae0de19360da6b4e21a0a0f8572f502f7e13d590186f8  lctype.c
c800311872ecba642f6d013b290eda83ba6cd8df8061e25cdca1dd4adcbed8b5  lctype.h
06d065d1f26c29546cc74b61a50267cb70567ae538691f190bf15ca5cd29d308  ldebug.c
b0fd38f6461251ec481cf5633d622569cef939067e37cac23331d9f96b601702  ldebug.h
af1522801876fe267bf2f892caa24209c395459331ac9a1fdf7ad3ed82402a6c  ldo.c
a577744f7c773bd91263540a1effefe75d3dfbfc8a9874349844f9a1ddae8d52  ldo.h
3c05402c5e282e06c2218f2469b1f38e1b84c783b9387aec7740c60b287d82c9  ldump.c
38809428b76fafa79db9f65b7248cc0c02c9b696e7410fc4ab1699940768f1b2  lfunc.c
81dc02d55ae8000461ab90bb9689d2600ba51f2ea9ac9fb019386995c07e3e4a  lfunc.h
caa4df2eccfae0899686d1aed3f2bad91184de3bd12176fdfead91147f704e0b  lgc.c
c83d3b8e31345c0aacd37442954a7f2434312ee48f98a8b9ae4fc8aad26d0c99  lgc.h
6c3e9858b9dad03ccd61445ab0bff0eb6ddfa5e731e044f93350fe6c6b2ad8d5  ljumptab.h
5cfbaabf3dc0d75835aaafc37fcd87a92f3dba451e002732747b602fda122958  llex.c
5ce0302528260ed6a40ec3ebf5e27d3bc108242d9dca57e8cdbe80fe34e13410  llex.h
d588bdac3c8e1adc88df3dc92c950a1bfa29663d15f9221f58e2aa553b05a422  llimits.h
da5c7d74cffce22f0a9eb468e6e7e80b543ea6bec5c21aefb8cf285a3556722a  lmathlib.c
6154c2865bec97e61a92d8284be7e8ba21818f6edc95f5d72adb5dcf3e9532ca  lmem.c
8318a170a1d4bf21c3cb4deb1f8661f1dc7cf4eb688e4ec9a1e7ea16bfc3b5cf  lmem.h
7a9748e38c52e45bc468cae74ccea08b98f8c53a523c1b0f9a117e5cf162c921  lobject.c
8539beae7dd9a08efc09b04d64c09663e9afc0f2ad342abc612bb000c0c9be70  lobject.h
bdc9d39fd8bf1a350c0249f726c5f61bef64b122b93a0fbc172b0328dc01f594  lopcodes.c
b3f511933be3039a4b92ba79601b2e5daf75b34f268bfc757adf3e24ea4f1ec9  lopcodes.h
fbdbebc96b136efc6165cba1ac2b311b0c52413aaef59ae9ec750748f13c9e9a  lopnames.h
e24f9e869afd690749404b3075fd82efaf3af882a13a131b612c6ff28e052a8c  lparser.c
68c34ef2d6e5a227f7cff18436a42c6739b1470ce12f059f168d9bb69fc8d49c  lparser.h
626785d4eda75e9435f0ac21a780ae3b11f8b27dc36934e637a5c2305f906531  lprefix.h
e5f3b17104f6764bad609d18ce8f3008a87731491a41cb78b79aed78777c21d8  lstate.c
8816454aa1df7c6cb1bc6366ca163b575e259e36497b75db354c572a81836cee  lstate.h
b6162f3a65735a902911ffafcafe1db0994c3b0df441581dcbb04dca6bc0b71d  lstring.c
a8cb0d737276f6a41071a0776ee1d8aa13cf02fa7c79ea096d202b14ea792e31  lstring.h
d386f8bbb75d5ef7b04c8ca9e747ff256525a9546bb6ff01c788b912b6995aa6  lstrlib.c
27a711276b1e22aaea3b9182e2d17693f598cfe9bc7fd203c05d400df6673853  ltable.c
1c8736b87aa408ef2afebfe693f46ae5424d658d798fb2a8a88b6868cf5569b5  ltable.h
3d430a6f9cde66d0b791444c6164e7559ccf62b7cdc3a6325882408c6806e26c  ltablib.c
7cbe645ed7e1161d31eb32ea467d8220b592c0acfa6107f0194487f0ddbf954b  ltm.c
5b2f6e59951edeef6e71bebe5ac4351f4b3602ee0461729308b80dcee8cea910  ltm.h
06138352a12f0710bc214eb3a2e134889979cadc9a7a761a4e43dafa926419af  lua.h
af243c94bee6d2601383e8b65ed66a0d1b4ab3b7418ff903fcd0679912160e3a  luaconf.h
4ea0f67d3e467f5d6a327616e22a3c2da8e03d47a3c3660916275f2842f521ea  lualib.h
7f7b078026e162161d402feba73a69cbe66a98558ec0745a445a595f42eac688  lundump.c
1137c0c9d8654ce223374d21c52a599fd1cc1ccb869801f87a27238a7bf55e8e  lundump.h
441251bdf176e066e60a36735594f0cb92c067143fed32e373949bdb65a52866  lutf8lib.c
7df5095ee2f6c88162d3536afb1fa04527b98fa3265cb579fc9aa6dcf05606de  lvm.c
aba601b887236d5e135392e0c49370df3b5a9ac5c66ab2d44156b3244f2dcef3  lvm.h
fba61ceeeb257a3fe5f63310f21e14fc26c4be647583dcf8e74cbc7fb7e43e8a  lzio.c
9673f62eac2d0fe3010f9d23148be7b8113319fdf4c4490015cd9feae2ecaad4  lzio.h
```

## Updating

Replace the files from a newer 5.4.x tarball whose checksum matches lua.org's
list, reapply the change above, and record the new version, hashes and date
here. A 5.5 release changes the C API (`lua_newstate` takes a seed) and needs
the runtime reviewed, not just copied.
