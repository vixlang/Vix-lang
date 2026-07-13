; ModuleID = 'vixc0'
source_filename = "vixc0"
target triple = "x86_64-pc-linux-gnu"

%ImportedExpr = type { i32, i32, ptr, ptr, double }

@.strlit3 = internal constant [1 x i8] zeroinitializer
@.strlit9 = internal constant [1 x i8] zeroinitializer
@.strlit28 = internal constant [1 x i8] zeroinitializer
@.strlit34 = internal constant [1 x i8] zeroinitializer

declare i32 @printf(ptr, ...)

declare ptr @malloc(i64)

declare ptr @calloc(i64, i64)

declare i32 @strcmp(ptr, ptr)

declare i32 @strlen(ptr)

define %ImportedExpr @ImportedNum(i32 %0) {
entry:
  %value = alloca i32, align 4
  store i32 %0, ptr %value, align 4
  %load1 = load i32, ptr %value, align 4
  %insert2 = insertvalue %ImportedExpr { i32 0, i32 undef, ptr undef, ptr undef, double undef }, i32 %load1, 1
  %insert4 = insertvalue %ImportedExpr %insert2, ptr @.strlit3, 2
  %insert5 = insertvalue %ImportedExpr %insert4, ptr null, 3
  %insert6 = insertvalue %ImportedExpr %insert5, double 0.000000e+00, 4
  ret %ImportedExpr %insert6
}

define %ImportedExpr @ImportedNil() {
entry:
  ret %ImportedExpr { i32 1, i32 0, ptr @.strlit9, ptr null, double 0.000000e+00 }
}

define i32 @eval(%ImportedExpr %0) {
entry:
  %e = alloca %ImportedExpr, align 8
  store %ImportedExpr %0, ptr %e, align 8
  %load13 = load %ImportedExpr, ptr %e, align 8
  %extract14 = extractvalue %ImportedExpr %load13, 0
  %cmp15 = icmp eq i32 %extract14, 0
  %bool16 = zext i1 %cmp15 to i32
  %cond17 = icmp ne i32 %bool16, 0
  br i1 %cond17, label %if.then0, label %if.else1

if.then0:                                         ; preds = %entry
  %load18 = load %ImportedExpr, ptr %e, align 8
  %extract19 = extractvalue %ImportedExpr %load18, 1
  %n = alloca i32, align 4
  store i32 %extract19, ptr %n, align 4
  %load20 = load i32, ptr %n, align 4
  ret i32 %load20

if.else1:                                         ; preds = %entry
  %load21 = load %ImportedExpr, ptr %e, align 8
  %extract22 = extractvalue %ImportedExpr %load21, 0
  %cmp23 = icmp eq i32 %extract22, 1
  %bool24 = zext i1 %cmp23 to i32
  %cond25 = icmp ne i32 %bool24, 0
  br i1 %cond25, label %if.then3, label %if.else4

if.end2:                                          ; preds = %if.end5
  ret i32 0

if.then3:                                         ; preds = %if.else1
  ret i32 0

if.else4:                                         ; preds = %if.else1
  br label %if.end5

if.end5:                                          ; preds = %if.else4
  br label %if.end2
}

define i32 @main() {
entry:
  %x = alloca %ImportedExpr, align 8
  store %ImportedExpr { i32 0, i32 42, ptr @.strlit28, ptr null, double 0.000000e+00 }, ptr %x, align 8
  %y = alloca %ImportedExpr, align 8
  store %ImportedExpr { i32 1, i32 0, ptr @.strlit34, ptr null, double 0.000000e+00 }, ptr %y, align 8
  %load38 = load %ImportedExpr, ptr %x, align 8
  %call39 = call i32 @eval(%ImportedExpr %load38)
  %load40 = load %ImportedExpr, ptr %y, align 8
  %call41 = call i32 @eval(%ImportedExpr %load40)
  %add42 = add i32 %call39, %call41
  ret i32 %add42
}

