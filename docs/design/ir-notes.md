# IR ノート(フェーズ1の確認結果)

`docs/design/ir-experiments/` の手書き `.ll` を clang 18 でビルドして確かめた内容です。
コンパイラ(`zevic0`)が出力する形の見本になります。

| 確認したこと | ファイル | 結果 |
| --- | --- | --- |
| 最小の Hello World(`ptr` の opaque pointer) | `hello.ll` | `clang hello.ll` で動く |
| ランタイム(C)を呼ぶ | `rt_call.ll` | `clang a.ll zevic_rt.c` で結合できる。Panic は終了コード 101 |
| `I32` の加算とオーバーフロー検査 | `overflow.ll` | `llvm.sadd.with.overflow.i32` → 起きたら `zevic_panic` |
| `String` = `{ ptr, i64 }` | `string_enum.ll` | 定数と `extractvalue` で扱える |
| `enum` = `{ i32 tag, [N x i64] }` | `string_enum.ll` | ペイロードの領域に、バリアントの構造体を `store`/`load` できる |
| `interface` 値 = `{ ptr, ptr }` | `iface_closure.ll` | 関数表の先頭から関数を読んで間接呼び出し |
| クロージャ = `{ ptr fn, ptr env }` | `iface_closure.ll` | 関数の第1引数が環境 |

## 実装に反映する決まり

- モジュールには `target triple` を書かない。`clang` に渡すと警告が出るので、ドライバが `-Wno-override-module` を付ける
- `enum` のペイロードは `[N x i64]`(N は最大のバリアントを 8 バイト単位に切り上げたもの)。ペイロードが無いときは `{ i32 }`
- 関数に渡す `this` は常に `ptr`(`class` は参照そのもの、`struct`/`enum` は値の置き場所)
- ランタイムへ渡す `String` などの集合体は、**ポインタ渡し**。戻り値は先頭の出力ポインタで受ける(ABI の差を避けるため)
- `alloca` はすべて関数の入口ブロックにまとめる(ループ内で増やさない)
- `unreachable` や `ret` のあとに命令が続かないよう、終端のあとは新しい(到達不能な)ブロックを開く
