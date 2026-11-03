# 提案 0002: 非同期 `async { }` と `await`、`Zevic.Async`

- 状態: 採用(2026-11-03、実装は同日。型名は `Async.Task<T>` のように `Async.` を付けて書く)
- 日付: 2026-11-03

## 動機

HTTP(提案 0003)や`Process`のように、待ち時間の長い処理を並べて走らせたい。
今は1本のスレッドで順に待つしかない。

## 提案

### 言語: 2つのキーワード

```
let a: Task<I32> = async { slowSquare(3) }
let b: Task<I32> = async { slowSquare(4) }
let total: I32 = await a + await b
```

- `async { ... }` は式で、型は `Task<T>`(`T` はブロックの値の型)。ブロックはすぐに別のスレッドで走り始める
- `await e` は前置の演算子(`!` と同じ強さ)。`e` は `Task<T>` で、終わるまで待って `T` を返す。`e.Await()` と同じ
- どちらも**糖衣**で、`async { b }` は `Async.Spawn(() => { b })`、`await e` は `e.Await()` に展開される。関数に`async`を付ける色分けは入れない。非同期の関数は、`Task<T>`を返す普通の関数にする
- `async` と `await` は予約語になる。既存のコードでこの名前を使っているものは、バッククォートで書く
- ブロックの中の `return` `break` `?` は、ラムダと同じで、ブロックを抜けるだけ

### 標準ライブラリ: `Zevic.Async`

```
class Task<T> {
    fn Await() -> T              // 2回呼んでも同じ値が返る
    fn IsDone() -> Bool
}
fn Spawn<T>(f: fn() -> T) -> Task<T>

class Channel<T> {
    fn Init(capacity: I64)       // 0以下は無制限
    mut fn Send(v: T)            // いっぱいなら待つ。閉じた後はPanic
    mut fn Recv() -> Option<T>   // 空なら待つ。閉じて空ならNone
    mut fn Close()
}

class Mutex {
    fn Init()
    fn Lock()
    fn Unlock()
}
```

### 実行の仕組み

- `Task` は OS のスレッド(POSIX のpthread)1本。Boehm GC をスレッド対応でビルドし、実行時の `GC_pthread_create` で登録する
- スレッド間で渡せるものに型の制限はない(参照を共有する)。データ競合を避けるのは書く人の責任で、そのための道具が `Channel` と `Mutex`。ただし1つだけ、コンパイラが守る:**`async` ブロックの中から、外側の関数の `mut` の局所変数(クラス以外の値)を使えない**(エラー)。`count += 1` のような事故が一番多いから。クラスの値は参照として共有できる(`Channel` や `Mutex` を持つクラスのため)
- `Task` の中で`Panic`が起きたら、今までと同じようにプログラム全体が止まる(そのスレッドだけを止める仕組みは、今は入れない)
- `main` が終わったら、走っているスレッドを待たずにプログラムは終わる

## 他の案

- 状態機械への変換(Rust/Kotlin風の`async fn`): 1パスのコード生成には大きすぎる。関数の色分けも持ち込む
- 1本のスレッドの上のコルーチン: Boehm GC が別のスタックを走査できない。将来、GC を差し替えるときに再検討する
- `Spawn` だけで構文は足さない: それでも動くが、`async {}` と `await` は短く読みやすいので足す

## 影響

- 字句: 予約語 `async` `await`。構文: 前置 `await`、式としての `async` ブロック
- ランタイム: スレッド対応の GC、スレッドと mutex と条件変数の薄い C 関数。リンクに `-lpthread`(必要な環境のみ)
- 標準ライブラリ: `Zevic.Async` を足す
- `zevfmt`: `await` は後ろに空白、`async {` も同様
- Windows のスレッドは未検証(CI を回してから確かめる)

## テスト

- ゴールデン: 2つの`async`を並べて結果を足す、`Channel`で生産者と消費者、`Mutex`で数えたカウンタ、`mut`の局所変数を使うとエラー
- 速さの確認: 100ミリ秒の`Sleep`を10個並べた全体が、1秒でなく数百ミリ秒で終わる
