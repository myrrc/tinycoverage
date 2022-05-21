let SessionLoad = 1
let s:so_save = &g:so | let s:siso_save = &g:siso | setg so=0 siso=0 | setl so=-1 siso=-1
let v:this_session=expand("<sfile>:p")
silent only
silent tabonly
cd ~/tinycoverage
if expand('%') == '' && !&modified && line('$') <= 1 && getline(1) == ''
  let s:wipebuf = bufnr('%')
endif
let s:shortmess_save = &shortmess
set shortmess=aoO
badd +4 ~/tinycoverage/test/main.cpp
badd +1 ~/tinycoverage/test/foo.h
badd +3 ~/tinycoverage/test/bar/bar.h
badd +2 ~/tinycoverage/test/bar/bar.cpp
badd +5 ~/tinycoverage/test/CMakeLists.txt
badd +9 ~/tinycoverage/run.sh
badd +1 ~/tinycoverage/gcno/bar.cpp.gcno
badd +2 ~/tinycoverage/tinycoverage/tinycoverage.h
badd +36 ~/tinycoverage/tinycoverage/tinycoverage.cpp
badd +3 ~/tinycoverage/.clang-format
badd +2 ~/tinycoverage/report
badd +135 ~/tinycoverage/parser.py
badd +4 ~/tinycoverage/gcno/tinycoverage.cpp.gcno
badd +1 ~/tinycoverage/gcno/main.cpp.gcno
badd +1 ~/tinycoverage/gcno/CMakeCXXCompilerId.gcno
badd +124 /usr/include/c++/10/cstdlib
badd +5 ~/tinycoverage/CMakeLists.txt
badd +3 ~/tinycoverage/tinycoverage/CMakeLists.txt
argglobal
%argdel
tabnew +setlocal\ bufhidden=wipe
tabnew +setlocal\ bufhidden=wipe
tabnew +setlocal\ bufhidden=wipe
tabrewind
edit ~/tinycoverage/run.sh
argglobal
balt ~/tinycoverage/test/CMakeLists.txt
setlocal fdm=expr
setlocal fde=nvim_treesitter#foldexpr()
setlocal fmr={{{,}}}
setlocal fdi=#
setlocal fdl=99
setlocal fml=1
setlocal fdn=20
setlocal fen
let s:l = 5 - ((4 * winheight(0) + 17) / 34)
if s:l < 1 | let s:l = 1 | endif
keepjumps exe s:l
normal! zt
keepjumps 5
normal! 028|
tabnext
edit ~/tinycoverage/tinycoverage/tinycoverage.cpp
argglobal
balt ~/tinycoverage/tinycoverage/tinycoverage.h
setlocal fdm=expr
setlocal fde=nvim_treesitter#foldexpr()
setlocal fmr={{{,}}}
setlocal fdi=#
setlocal fdl=99
setlocal fml=1
setlocal fdn=20
setlocal fen
28
normal! zo
30
normal! zo
36
normal! zo
75
normal! zo
let s:l = 1 - ((0 * winheight(0) + 17) / 34)
if s:l < 1 | let s:l = 1 | endif
keepjumps exe s:l
normal! zt
keepjumps 1
normal! 025|
tabnext
edit ~/tinycoverage/tinycoverage/tinycoverage.h
argglobal
balt ~/tinycoverage/CMakeLists.txt
setlocal fdm=expr
setlocal fde=nvim_treesitter#foldexpr()
setlocal fmr={{{,}}}
setlocal fdi=#
setlocal fdl=99
setlocal fml=1
setlocal fdn=20
setlocal fen
let s:l = 1 - ((0 * winheight(0) + 17) / 34)
if s:l < 1 | let s:l = 1 | endif
keepjumps exe s:l
normal! zt
keepjumps 1
normal! 0
tabnext
edit ~/tinycoverage/tinycoverage/tinycoverage.cpp
argglobal
balt ~/tinycoverage/tinycoverage/CMakeLists.txt
setlocal fdm=expr
setlocal fde=nvim_treesitter#foldexpr()
setlocal fmr={{{,}}}
setlocal fdi=#
setlocal fdl=99
setlocal fml=1
setlocal fdn=20
setlocal fen
28
normal! zo
30
normal! zo
36
normal! zo
75
normal! zo
let s:l = 84 - ((33 * winheight(0) + 17) / 34)
if s:l < 1 | let s:l = 1 | endif
keepjumps exe s:l
normal! zt
keepjumps 84
normal! 0
tabnext 3
if exists('s:wipebuf') && len(win_findbuf(s:wipebuf)) == 0 && getbufvar(s:wipebuf, '&buftype') isnot# 'terminal'
  silent exe 'bwipe ' . s:wipebuf
endif
unlet! s:wipebuf
set winheight=1 winwidth=20
let &shortmess = s:shortmess_save
let s:sx = expand("<sfile>:p:r")."x.vim"
if filereadable(s:sx)
  exe "source " . fnameescape(s:sx)
endif
let &g:so = s:so_save | let &g:siso = s:siso_save
set hlsearch
nohlsearch
let g:this_session = v:this_session
let g:this_obsession = v:this_session
doautoall SessionLoadPost
unlet SessionLoad
" vim: set ft=vim :
