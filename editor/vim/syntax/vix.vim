if exists("b:current_syntax")
  finish
endif

syn case match

" Comments
syn keyword vixTodo TODO FIXME XXX NOTE HACK contained
syn match vixComment "//.*$" contains=vixTodo
syn region vixComment start="/\*" end="\*/" contains=vixTodo,vixComment

" Strings
syn region vixString start='"' skip='\\"' end='"'

" Char literals
syn match vixChar "'[^'\\]'"
syn match vixChar "'\\[ntr0\\'']'"
syn match vixChar "'\\[0-9][0-9][0-9]'"

" Numbers
syn match vixNumber '\<-\?[0-9][0-9_]*\>'
syn match vixFloat '\<-\?[0-9][0-9_]*\.[0-9][0-9_]*\>'
syn match vixHex '\<0[xX][0-9a-fA-F][0-9a-fA-F_]*\>'

" Types
syn keyword vixType i8 i16 i32 i64 u8 u16 u32 u64 f32 f64
syn keyword vixType string void bool usize isize nil ptr

" Booleans
syn keyword vixBool true false

" Self
syn keyword vixSelf self

" Keywords
syn keyword vixKeyword fn return let mut if elif else while for in
syn keyword vixKeyword match struct type pub import extern impl
syn keyword vixKeyword break continue and or print as macro

" Function definition: the name after `fn`
syn match vixFuncName '\<fn\>\s*\zs[a-zA-Z_][a-zA-Z0-9_]*'
syn match vixMacroName '\<macro\>\s*\$\?\zs[a-zA-Z_][a-zA-Z0-9_]*'

" Type definition: the name after `type`, `struct`, `impl`
syn match vixTypeName '\<type\>\s*\zs[a-zA-Z_][a-zA-Z0-9_]*'
syn match vixTypeName '\<struct\>\s*\zs[a-zA-Z_][a-zA-Z0-9_]*'
syn match vixTypeName '\<impl\>\s*\zs[a-zA-Z_][a-zA-Z0-9_]*'

" Import: the module name after `import`
syn match vixImportName '\<import\>\s*\zs[a-zA-Z_./][a-zA-Z0-9_./]*'

" Generic parameters :[T] or :[T, E]
syn match vixGeneric ':\[[A-Za-z_][A-Za-z0-9_, ]*\]'

" Optional type ?T
syn match vixOptional '?[A-Za-z_][A-Za-z0-9_]*'

" Attributes #[...]
syn match vixAttribute '#\[.\{-}\]'

" Operators
syn match vixOperator '=>\|==\|!=\|<=\|>=\|+=\|-=\|->\|\.\.\.\|\.\.'
syn match vixOperator '[-+*%=<>!&|^~;]'

" Delimiters
syn match vixDelimiter '[{}()\[\],:]'

" ADT constructors (capitalized)
syn match vixADT '\<[A-Z][a-zA-Z0-9_]*\>'

hi def link vixTodo        Todo
hi def vixComment guifg=#6c9e31 ctermfg=green
hi def link vixString      String
hi def link vixChar        String
hi def link vixNumber      Number
hi def link vixFloat       Float
hi def link vixHex         Number
hi def link vixType        Type
hi def link vixBool        Boolean
hi def link vixSelf        Identifier
hi def link vixKeyword     Keyword
hi def link vixFuncName    Function
hi def link vixMacroName   Macro
hi def link vixTypeName    Type
hi def link vixImportName  String
hi def link vixGeneric     Special
hi def link vixOptional    Type
hi def link vixAttribute   PreProc
hi def link vixOperator    Operator
hi def link vixDelimiter   Delimiter
hi def link vixADT         Type

let b:current_syntax = "vix"
