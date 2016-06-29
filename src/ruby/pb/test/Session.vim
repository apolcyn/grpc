let SessionLoad = 1
if &cp | set nocp | endif
let s:cpo_save=&cpo
set cpo&vim
vmap gx <Plug>NetrwBrowseXVis
nmap gx <Plug>NetrwBrowseX
vnoremap <silent> <Plug>NetrwBrowseXVis :call netrw#BrowseXVis()
nnoremap <silent> <Plug>NetrwBrowseX :call netrw#BrowseX(expand((exists("g:netrw_gx")? g:netrw_gx : '<cfile>')),netrw#CheckIfRemote())
let &cpo=s:cpo_save
unlet s:cpo_save
set autoindent
set backspace=indent,eol,start
set backupdir=~/.vim/cache,.,~/tmp,~/
set directory=~/.vim/cache//,.,~/tmp,/var/tmp,/tmp
set fileencodings=ucs-bom,utf-8,default,latin1
set formatoptions=tcqr
set helplang=en
set hidden
set makeprg=/usr/bin/blaze\ build\ --color=no\ --curses=no
set printoptions=paper:letter
set ruler
set runtimepath=~/.vim,/usr/share/vim/google/libgit,/usr/share/vim/google/legacy,/usr/share/vim/google/whitespace,/usr/share/vim/google/vcscommand-g4,/usr/share/vim/google/selector,/usr/share/vim/google/safetmpdirs,/usr/share/vim/google/relatedfiles,/usr/share/vim/google/piper,/usr/share/vim/google/googlespell,/usr/share/vim/google/piperlib,/usr/share/vim/google/googlestyle,/usr/share/vim/google/googlepaths,/usr/share/vim/google/critique,/usr/share/vim/google/codefmt-google,/usr/share/vim/google/codefmt,/usr/share/vim/google/clang-format,/usr/share/vim/google/autogen,/usr/share/vim/google/google-filetypes,/usr/share/vim/google/ft-soy,/usr/share/vim/google/ft-python,/usr/share/vim/google/ft-proto,/usr/share/vim/google/ft-javascript,/usr/share/vim/google/ft-java,/usr/share/vim/google/ft-gss,/usr/share/vim/google/ft-cpp,/usr/share/vim/google/googler,/usr/share/vim/google/compatibility,/usr/share/vim/google/glaive,/usr/share/vim/google/googlelib,/usr/share/vim/google/logmsgs,/var/lib/vim/addons,/usr/share/vim/vimfiles,/usr/share/vim/vim74,/usr/share/vim/vimfiles/after,/var/lib/vim/addons/after,~/.vim/after,/usr/share/vim/google/glug,/usr/share/vim/google/maktaba
set shiftwidth=2
set smarttab
set suffixes=.bak,~,.swp,.o,.info,.aux,.log,.dvi,.bbl,.blg,.brf,.cb,.ind,.idx,.ilg,.inx,.out,.toc
set undodir=~/.vim/cache,.
let s:so_save = &so | let s:siso_save = &siso | set so=0 siso=0
let v:this_session=expand("<sfile>:p")
silent only
cd ~/grpc_workspace/grpc/src/ruby/pb/test
if expand('%') == '' && !&modified && line('$') <= 1 && getline(1) == ''
  let s:wipebuf = bufnr('%')
endif
set shortmess=aoO
badd +1 client.rb
argglobal
silent! argdel *
argadd client.rb
edit client.rb
set splitbelow splitright
set nosplitbelow
set nosplitright
wincmd t
set winheight=1 winwidth=1
argglobal
nnoremap <buffer> <silent> g} :exe        "ptjump =RubyCursorIdentifier()"
nnoremap <buffer> <silent> } :exe          "ptag =RubyCursorIdentifier()"
nnoremap <buffer> <silent> g] :exe      "stselect =RubyCursorIdentifier()"
nnoremap <buffer> <silent> g :exe        "stjump =RubyCursorIdentifier()"
nnoremap <buffer> <silent>  :exe v:count1."stag =RubyCursorIdentifier()"
nnoremap <buffer> <silent> ] :exe v:count1."stag =RubyCursorIdentifier()"
nnoremap <buffer> <silent>  :exe  v:count1."tag =RubyCursorIdentifier()"
nnoremap <buffer> <silent> g] :exe       "tselect =RubyCursorIdentifier()"
nnoremap <buffer> <silent> g :exe         "tjump =RubyCursorIdentifier()"
setlocal keymap=
setlocal noarabic
setlocal autoindent
setlocal backupcopy=
setlocal balloonexpr=RubyBalloonexpr()
setlocal nobinary
setlocal nobreakindent
setlocal breakindentopt=
setlocal bufhidden=
setlocal buflisted
setlocal buftype=
setlocal nocindent
setlocal cinkeys=0{,0},0),:,0#,!^F,o,O,e
setlocal cinoptions=
setlocal cinwords=if,else,while,do,for,switch
setlocal colorcolumn=
setlocal comments=:#
setlocal commentstring=#\ %s
setlocal complete=.,w,b,u,t,i
setlocal concealcursor=
setlocal conceallevel=0
setlocal completefunc=
setlocal nocopyindent
setlocal cryptmethod=
setlocal nocursorbind
setlocal nocursorcolumn
setlocal nocursorline
setlocal define=
setlocal dictionary=
setlocal nodiff
setlocal equalprg=
setlocal errorformat=
setlocal noexpandtab
if &filetype != 'ruby'
setlocal filetype=ruby
endif
setlocal fixendofline
setlocal foldcolumn=0
setlocal foldenable
setlocal foldexpr=0
setlocal foldignore=#
setlocal foldlevel=0
setlocal foldmarker={{{,}}}
setlocal foldmethod=manual
setlocal foldminlines=1
setlocal foldnestmax=20
setlocal foldtext=foldtext()
setlocal formatexpr=
setlocal formatoptions=croql
setlocal formatlistpat=^\\s*\\d\\+[\\]:.)}\\t\ ]\\s*
setlocal grepprg=
setlocal iminsert=2
setlocal imsearch=2
setlocal include=^\\s*\\<\\(load\\>\\|require\\>\\|autoload\\s*:\\=[\"']\\=\\h\\w*[\"']\\=,\\)
setlocal includeexpr=substitute(substitute(v:fname,'::','/','g'),'$','.rb','')
setlocal indentexpr=GetRubyIndent(v:lnum)
setlocal indentkeys=0{,0},0),0],!^F,o,O,e,=end,=else,=elsif,=when,=ensure,=rescue,==begin,==end
setlocal noinfercase
setlocal iskeyword=@,48-57,_,192-255
setlocal keywordprg=ri
setlocal nolinebreak
setlocal nolisp
setlocal lispwords=
setlocal nolist
setlocal makeprg=
setlocal matchpairs=(:),{:},[:]
setlocal modeline
setlocal modifiable
setlocal nrformats=bin,octal,hex
set number
setlocal number
setlocal numberwidth=4
setlocal omnifunc=rubycomplete#Complete
setlocal path=~/.rvm/gems/ruby-2.3.0@global/gems/did_you_mean-1.0.0/lib,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby/2.3.0,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby/2.3.0/x86_64-linux,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby/2.3.0,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby/2.3.0/x86_64-linux,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby,~/.rvm/rubies/ruby-2.3.0/lib/ruby/2.3.0,~/.rvm/rubies/ruby-2.3.0/lib/ruby/2.3.0/x86_64-linux
setlocal nopreserveindent
setlocal nopreviewwindow
setlocal quoteescape=\\
setlocal noreadonly
setlocal norelativenumber
setlocal norightleft
setlocal rightleftcmd=search
setlocal noscrollbind
setlocal shiftwidth=2
setlocal noshortname
setlocal nosmartindent
setlocal softtabstop=0
setlocal nospell
setlocal spellcapcheck=[.?!]\\_[\\])'\"\	\ ]\\+
setlocal spellfile=
setlocal spelllang=en
setlocal statusline=
setlocal suffixesadd=.rb
setlocal swapfile
setlocal synmaxcol=3000
if &syntax != 'ruby'
setlocal syntax=ruby
endif
setlocal tabstop=8
setlocal tagcase=
setlocal tags=./tags,./TAGS,tags,TAGS,~/.rvm/gems/ruby-2.3.0@global/gems/did_you_mean-1.0.0/lib/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby/2.3.0/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby/2.3.0/x86_64-linux/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/site_ruby/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby/2.3.0/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby/2.3.0/x86_64-linux/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/vendor_ruby/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/2.3.0/tags,~/.rvm/rubies/ruby-2.3.0/lib/ruby/2.3.0/x86_64-linux/tags
setlocal textwidth=0
setlocal thesaurus=
setlocal noundofile
setlocal undolevels=-123456
setlocal nowinfixheight
setlocal nowinfixwidth
setlocal wrap
setlocal wrapmargin=0
silent! normal! zE
let s:l = 419 - ((42 * winheight(0) + 41) / 83)
if s:l < 1 | let s:l = 1 | endif
exe s:l
normal! zt
419
normal! 020|
tabnext 1
if exists('s:wipebuf')
  silent exe 'bwipe ' . s:wipebuf
endif
unlet! s:wipebuf
set winheight=1 winwidth=20 shortmess=filnxtToO
let s:sx = expand("<sfile>:p:r")."x.vim"
if file_readable(s:sx)
  exe "source " . fnameescape(s:sx)
endif
let &so = s:so_save | let &siso = s:siso_save
doautoall SessionLoadPost
unlet SessionLoad
" vim: set ft=vim :
