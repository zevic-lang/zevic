# セルフホスト(段階⑦)の進み具合

`tools/zevic1/` は、Zevic で書いた `zevic0` です。`zevic0` でコンパイルして動かし、`zevic0` と出力を比べる(差分テスト、`make test`)。

## できたもの

| zevic0 | zevic1 | 差分テスト |
| --- | --- | --- |
| `lexer.c` | `Lexer.zev` `Token.zev` | `--dump-tokens` が全 `.zev`(エラーを含む)で一致 |
| `ast.c` `parser.c` | `Ast.zev` `Parser.zev` | `--dump-ast` が一致。ユニオン型(`A \| B`、`is`、型パターン)も |
| `module.c` | `Home.zev` `Resolve.zev` | `--dump-modules`(import の解決、循環、エラー)が一致 |

差分テストは 501 件(`tests/zevic1_test.py`)。

## これから

1. `types.c`(型の表現、配置、intern)→ `Types.zev`。`--dump-layout` のような出力を足して差分テストにする
2. `gen.c`(約 6000 行。型検査と IR 生成が1パス)を段階に分けて移す: 式と文 → 関数とジェネリクスの単相化 → 型の定義(`struct` `class` `enum` ユニオン)→ `match` と網羅性 → クロージャ・`defer`・`async` → 標準ライブラリの組み込み
3. `driver.c`(clang の呼び出し、ランタイムと GC のキャッシュ)
4. ブートストラップ: `zevic1` が自分をコンパイルした `zevic2` の `.ll` と、`zevic1` の `.ll` が一致する。`bootstrap/zevic.ll` をコミットする

`Same(a, b)`(仕様にあるが zevic0 に未実装)は、zevic1 でモジュールの同一判定に要るので、提案を書いて zevic0 にも入れる。
