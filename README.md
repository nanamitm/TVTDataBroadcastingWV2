# TVTDataBroadcastingWV2

ダウンロード https://github.com/otya128/TVTDataBroadcastingWV2/releases

[web-bml](https://github.com/otya128/web-bml)とWebView2を使ったTVTest用データ放送プラグイン

![動作画面](https://user-images.githubusercontent.com/4075988/162745408-282fb7ab-9826-4e82-b2ab-b1ab347a42b4.png)

## 動作環境

* TVTest 0.9.0 正式版以上
* Windows 10以上
    * Windows 7用の対応は入れていないのでWindows 7では映像と正常に合成できないはず 8.xなら動くかも
* WebView2ランタイム
    * 最低限89.0.774.44以上である必要がある
    * もしインストールされていなければインストール
        * インストーラ: https://go.microsoft.com/fwlink/p/?LinkId=2124703
        * 配布ページ: https://developer.microsoft.com/ja-jp/microsoft-edge/webview2/#download-section
        * OSが32-bitでなければTVTestのアーキテクチャに関わらずx64で動作する `Plugins/TVTDataBroadcastingWV2/WebView2/msedgewebview2.exe` `Plugins/TVTDataBroadcastingWV2/WebView2/EBWebView/x64/EmbeddedBrowserWebView.dll` のように300MB以上, 200ファイル程度あるFixed Versionを直接配置しても可能
* Visual C++ 2015-2022ランタイム
    * 万が一入っていなければTVTestのアーキテクチャに合わせて https://aka.ms/vs/17/release/vc_redist.x64.exe (x64) https://aka.ms/vs/17/release/vc_redist.x86.exe (x86) からインストール

映像レンダラはEVR, EVR (Custom Presenter), madVR, システムデフォルト, VMR9, VMR9 Renderless, VMR7, VMR7 Renderlessで動作します。 ただし現時点ではVMR9 Renderless, VMR7 Renderlessを使うとフルスクリーンでの表示などに支障があります。

字幕やコメントを直接映像に合成するプラグインとは相性が悪いため、同時に正常に表示したい場合にはレイヤードウィンドウを使うように設定するかあきらめるなどしてください。
映像レンダラにVMR9 Renderless, VMR7 Renderlessを選択した場合映像に直接合成してもレイヤードウィンドウを使うようにしても字幕やコメントがデータ放送中の映像に合わせて縮小されます。

## 操作

TVTest起動時には有効にならないようになっているため右クリックメニューからプラグインを有効にするか、設定でサイドバーにプラグイン有効アイコンを表示させてそこから有効にしてください。
有効にしたタイミングでWebView2が起動します。

プラグイン有効時に表示されるリモコンかパネルに追加されるリモコンかTVTest側の設定でキーなどをデータ放送の操作に割り当てて操作することが出来ます。

字幕ボタンを押すと[aribb24.js](https://github.com/monyone/aribb24.js)を使った字幕を表示することが出来ます。

テレ東(BSや系列局含)では初回は50秒ほど待たないとデータ放送が表示されません。

## 設定

キー割り当て、パネル、サイドバー、ステータスバーの設定はTVTestの設定で行えます。

### 通信コンテンツ

Plugins/TVTDataBroadcastingWV2.iniを以下のようにすると通信が有効になります。

```ini
[TVTDataBroadcastingWV2]
EnableNetwork=1
```

### プラグイン有効時にリモコンを表示しない

パネルを使う場合やキー割り当てした場合リモコンウィンドウは不要

### TVTest起動時にプラグインを有効にする

### 音量をTVTestと連動する

操作音などの音量

### 数字ボタンが使われていなければTVTestに渡す

データ放送中で数字キーが使われていない場合数字コマンドで選局可能にする

### 字幕状態を起動時に復元する

設定ダイアログの「前回の字幕状態を起動時に復元する」(INI `RestoreCaptionState`)を有効にすると、字幕の有効/無効状態(`AutoEnableCaption`)を次回起動時に復元します。データ放送/コメント機能とは独立した設定です。

## ニコニコ実況コメント機能 (フォーク独自機能)

このフォークでは、ニコニコ実況/NX-Jikkyoのコメントを取得して動画に重ねて表示する機能を追加しています(NicoJK相当)。コメント自体の取得は外部ツール[jkcnsl](https://github.com/nanamitm/jkcnsl)(`jkcnsl.exe`をTVTest本体と同じフォルダに配置)が行い、本プラグインは表示・投稿・NGフィルタ等のUIを担当します。

### 有効化

コメント機能は本体のプラグイン有効/無効・通信機能の有効化とは別に、パネルレイアウト(リモコンパネル形式)上のチェックボタン「コメント」で切り替えます。右クリックメニューには表示されないため、TVTestのパネル設定でリモコンパネルを表示しておく必要があります。INIにも起動時のゲートとして`EnableComment`があります。

### 接続先

INI `[TVTDataBroadcastingWV2]`の`RefugeUri`(NX-Jikkyo等の避難所URI)、`chatStreamID`(通常のニコニコ実況のID)、`RefugeMixing`(両方を同時取得して混合表示)、`PostToRefuge`(投稿先選択)で接続方式を切り替えます。

### 投稿・ログイン

「勢い」パネル(WebView2)内に投稿欄があります。投稿するには「設定」ボタン(ツールチップ「ニコニコログイン」)からメール/パスワード(+ワンタイムパスワード)でログインしてください。未ログイン時は投稿欄が無効化されます。
投稿欄の「▷」ボタン(コマンド選択)から色/位置(上/下/流れる)/サイズをNicoJKと同様に選べます。「184」ボタンで匿名投稿を切り替えられます。

### NGフィルタ・置換

コメントログ上で対象行を右クリックすると、そのユーザをNGに追加/解除できます(INI `[NG]`に保存)。正規表現・コマンド単位のNGは`[NG]`を直接編集してください。
`[CustomReplace]`セクションに`s/パターン/置換文字列/g`形式のキーを追加すると、本文を正規表現置換した上で表示・ログ保存できます(置換はNGフィルタより先に適用されます)。

### 勢いパネル・過去ログ

「勢い」パネルは「勢い」(実況番号別の勢い表、列タップでソート、`MomentumSortColumn`/`MomentumSortAscending`に保存)と「ログ」(コメントの生ログ)タブを切り替えられます。

`LogfileMode`/`LogfileFolder`を設定すると、NicoJK互換形式(`{フォルダ}\jk{ID}\{Unix時刻10桁}.txt`、生の`<chat>`タグ)でコメントログを記録できます。録画再生時など放送波のTOT(放送時刻)が壁時計と60秒以上ずれている場合は自動的にタイムシフト再生と判断し、ライブ取得ではなくこのログから同期再生します。

### 表示設定

設定ダイアログから`CommentOpacity`(コメント透過率、0-100、既定100)と`CommentDuration`(コメント表示時間、1000-5000ms、既定4000)を変更できます。

### チャンネル対応

放送のNetworkID/ServiceIDからニコニコ実況chへの対応付けは、本プラグイン自身のINI `[Channels]`を優先し、未設定のキーのみ既存の`NicoJK.ini`の`[Channels]`セクション(配置先の2階層上)へフォールバックします。BS/CS/プレミアム等は実際のNetworkID、地上波は`0xF`をキーの先頭に使います。

## 制約

おおよそ実装されていますが一部のAPI、イベント、要素は未実装です。

通信機能は既定では無効であり、その場合すべての外部へのリクエストはブロックされます。(ICoreWebView2::add_WebResourceRequestedを呼んでいる部分を参照)

## ビルド方法

### TVTestプラグインのビルド

Visual C++ 2022が必要(2019でもおそらく可能)

NuGetパッケージを復元しTVTDataBroadcastingWV2.slnをビルド

### web-bmlのビルド

現状web-bmlを使うために無理やりサブモジュールで参照していてさらにそのweb-bmlもサブモジュールを持っているため以下のコマンドで初期化/更新

```sh
git submodule update --init --recursive
```

以下のコマンドでビルド

```sh
cd browser
npm i
npm -w @chinachu/aribts run build
npm run build
```

フォントをコピー
```bat
copy web-bml\fonts\*.woff2 dist\
```

* Plugins/
    * TVTDataBroadcastingWV2.tvtp
    * TVTDataBroadcastingWV2
        * resources/
            * TVTDataBroadcastingWV2.html
            * dist/
                * TVTDataBroadcastingWV2.js
                * Kosugi-Regular.ttf
                * KosugiMaru-Regular.ttf

のように配置するかTVTDataBroadcastingWV2.tvtpと同じディレクトリにTVTDataBroadcastingWV2.iniを作り以下のようにする

```ini
[TVTDataBroadcastingWV2]
ResourceDirectory=x:\xx\browser\
```

