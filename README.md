# Zevic

Zevic(仮称)は、Cの資産をそのまま使える、小さくて書きやすいネイティブ言語です。
ソースを **テキストの LLVM IR** に変換し、`clang` で機械語にします。

- 言語仕様: [`docs/spec/zevic-spec.md`](docs/spec/zevic-spec.md)
- 実装計画: [`docs/plan.md`](docs/plan.md)
- 最初のコンパイラ `zevic0` は C(C11)で書かれています(`zevic0/`)

```
module Main

import Zevic.Console

fn Main() {
    Console.Println("Hello, World!")
}
```

## ビルド

必要なもの: C コンパイラ(clang 推奨)、`make`、`clang` 15 以上(生成した IR のビルドに使用)、`python3`(テスト用)。

```sh
make            # build/zevic を作る
make test       # tests/ のゴールデンテストを実行
./build/zevic examples/01_hello.zev -o hello && ./hello
```

## 使い方

```sh
zevic hello.zev -o hello                # ビルド
zevic hello.zev native/a.c -o hello     # C のファイルも一緒に
zevic hello.zev --emit-ll -o hello.ll   # LLVM IR だけ出す
zevic --version
```

標準ライブラリ(`stdlib/`)とランタイム(`runtime/`)は、実行ファイルの隣の `share/zevic/` か、
環境変数 `ZEVIC_HOME` が指す場所から探します。リポジトリ直下で実行するときは追加の設定は不要です。

## リリース

`main` への push でビルドが成功するたびに、Linux / macOS / Windows の実行ファイルを
`.github/workflows/ci.yml` が GitHub Releases に公開します。

## 状態

仕様書は草案です。実装の進み具合は [`CHANGELOG.md`](CHANGELOG.md) を参照してください。

## ライセンス

Apache-2.0(`LICENSE`)。
