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

必要なもの: C コンパイラ、`make`、`python3`(テスト用)、そして **clang 15 以上**(`zevic` が生成した LLVM IR をビルドするために使います)。

### LLVM(clang)のインストール

| OS | コマンド |
| --- | --- |
| Ubuntu / Debian | `sudo apt install clang` |
| Windows | `winget install LLVM.LLVM`(MSVC のライブラリが必要なので Visual Studio Build Tools も入れてください) |
| macOS | `brew install llvm` |

`zevic` は次の順で clang を探します: `--cc` → 環境変数 `ZEVIC_CLANG` → `PATH` 上の `clang` →
よくあるインストール先(`C:\Program Files\LLVM\bin`、Homebrew、`/usr/lib/llvm-*/bin`)。
見つからないとき、または古いときは、上のインストール方法を表示して終了します。


```sh
make            # build/zevic と build/zevpm を作る(`make zevfmt` で整形ツール)
make test       # ゴールデンテスト、zevpm のテスト、examples を実行
./build/zevic examples/01_hello.zev -o hello && ./hello
```

## 使い方

```sh
zevic hello.zev -o hello                # ビルド
zevic hello.zev native/a.c -o hello     # C のファイルも一緒に(.c .cpp .o .a)
zevic app.zev -Iinc -DDEBUG -L. -lfoo   # -I -D -L -l はそのままコンパイラ/リンカへ
zevic app.zev --emit-header zevic.h     # export "C" した関数の C ヘッダも書き出す
zevic hello.zev --emit-ll -o hello.ll   # LLVM IR だけ出す
zevic --version
```

標準ライブラリ(`stdlib/`)とランタイム(`runtime/`)は、実行ファイルの隣の `share/zevic/` か、
環境変数 `ZEVIC_HOME` が指す場所から探します。リポジトリ直下で実行するときは追加の設定は不要です。

## zevpm(パッケージとビルド)

```sh
zevpm new myapp          # myapp/ に zevic.toml と src/Main.zev ができる(--lib でライブラリ)
cd myapp
zevpm run                # ビルドして実行(引数は zevpm run -- a b)
zevpm test               # test ブロックと _test.zev を実行(zevpm test 名前 で絞り込み)
zevpm get owner/name@v1.0.0   # git の依存を足す(zevic.lock に commit を記録、~/.zevpm にキャッシュ)
zevpm build --release    # build/ に出力(ライブラリは .a と include/<Name>.h)
```

`zevpm` は `zevic` を呼ぶので、同じディレクトリ(または `PATH`、環境変数 `ZEVIC`)に置いてください。git の依存には `git` が必要です。

## ランタイムとGC

ランタイムと Boehm GC(`runtime/third_party/bgc`)は C で書かれています。最初のビルドで一度だけコンパイルし、
`~/.cache/zevic`(Windows は `%LOCALAPPDATA%\zevic\cache`、`ZEVIC_CACHE` で変更可)に置いて使い回します。

## リリース

`main` への push でビルドが成功するたびに、Linux / macOS / Windows の実行ファイルを
`.github/workflows/ci.yml` が GitHub Releases に公開します。

## 例

- [`examples/`](examples/) — Hello、FizzBuzz、階乗、電卓、`interface`、C との連携(`06_c_interop/`)、ファイル、外部プログラム、統計、コレクション(`07`〜`10`)、`zevpm` パッケージ(`apps/wordcount/`)

## 状態

v0.3.0: 型、`struct`/`class`/`enum`、`interface`、ジェネリクス、クロージャ、`defer`、GC、C との連携、
`test` ブロック、`zevpm`、標準ライブラリ(`Fs` `Env` `Text` `Math` `Time` `Process` など)まで動きます。
`zevfmt`(整形: `zevfmt -w .`、検査: `zevfmt -check .`)も動きます。セルフホスティング(`zevic1`)はこれからです(計画は [`docs/plan.md`](docs/plan.md))。

仕様書は草案です。実装の進み具合は [`CHANGELOG.md`](CHANGELOG.md) を参照してください。

## ライセンス

Apache-2.0(`LICENSE`)。
