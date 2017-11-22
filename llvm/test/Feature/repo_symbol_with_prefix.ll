; RUN: llc -filetype=obj -debug-only repo-object %s -o /dev/null 2>&1 | FileCheck %s

; REQUIRES: asserts

source_filename = "test.c"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu-repo"

@.str = external unnamed_addr constant [13 x i8], align 1, !fragment !0

; Function Attrs: noinline nounwind optnone uwtable
declare !fragment !1 i32 @main()

!repo.tickets = !{!0, !1}

!0 = !TicketNode(name: ".str", digest: [16 x i8] c"v\C6>J]\C8h\C1\0F\C1\D2\F3\A7\9C5O", linkage: private)
!1 = !TicketNode(name: "main", digest: [16 x i8] c"\1F8bZ\ED\DB\85O\7F@\15\DA\C7\C6\0Cc", linkage: external)

;CHECK: ticket name '.L.str' digest '4f359ca7f3d2c10fc168c85d4a3ec676' adding.
;CHECK: ticket name 'main' digest '630cc6c7da15407f4f85dbed5a62381f' adding.
