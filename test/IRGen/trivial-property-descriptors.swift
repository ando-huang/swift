// RUN: %swift %use_no_opaque_pointers -disable-legacy-type-info -target x86_64-unknown-windows-msvc -parse-stdlib -module-name Swift -enable-library-evolution -S -emit-ir -o - %s | %FileCheck %s
// RUN: %swift -disable-legacy-type-info -target x86_64-unknown-windows-msvc -parse-stdlib -module-name Swift -enable-library-evolution -S -emit-ir -o - %s

public struct S {}
extension S {
  public var i: () { return () }
  public var j: () { return () }
}

// CHECK: @"$ss1SV1jytvpMV" = dllexport alias { i32 }, { i32 }* @"$ss1SV1iytvpMV"

