# TVTDataBroadcastingWV2

ダウンロード https://github.com/nanamitm/TVTDataBroadcastingWV2/releases

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

### 字幕

字幕ボタンを押すと[libaribcaption](https://github.com/xqq/libaribcaption)を使った字幕を表示できます。

字幕ストリームのTS/PES分離はweb-bml、ARIB STD-B24字幕のデコードとDirectWriteによる描画はネイティブ側のlibaribcaptionが担当します。aribb24.jsは直接使用していません。

libaribcaptionはGitサブモジュールとして取り込み、ソースをプラグイン本体に組み込んでビルドします。そのため、実行環境へlibaribcaptionを別途インストールする必要はありません。

#### 字幕の表示設定

フォントや縁取りなどの表示設定は プラグイン設定→「字幕の表示設定」 から行えます。設定項目とINIのキー名・既定値は[TVCaption3](https://github.com/xtne6f/TVCaption3)に合わせています。

| INIキー | 既定値 | 内容 |
| --- | --- | --- |
| `FaceName` / `FaceName1` / `FaceName2` | `MS Gothic` / 空 / 空 | 使用するフォントおよびフォールバックフォント名。空にすると指定しない |
| `StrokeWidth` | `30` | 字幕文の縁取りの幅の10倍。0より大きいとき常に縁取る |
| `OrnStrokeWidth` | `50` | ORN縁取り指定された字幕文の縁取りの幅の10倍。`StrokeWidth`が0より大きいときは無視される |
| `ShowFlags` / `ShowFlagsSuper` | `65535` | 字幕/文字スーパーを表示するかどうか。表示しない場合は`0` |
| `DelayTime` | `450` | 字幕を受け取ってから表示するまでの遅延時間(ミリ秒)。-5000から5000まで |
| `DelayTimeSuper` | `0` | 文字スーパーの遅延時間(ミリ秒)。ダイアログからは設定できない |
| `NoBackground` | `0` | 背景を常に透明にするかどうか |
| `ReplaceFullAlnum` | `1` | 英数字を半角置換するかどうか |
| `ReplaceFullJapanese` | `1` | 日本語の約物などを半角置換するかどうか |
| `ReplaceDrcs` | `0` | DRCS図形を文字に置換するかどうか |
| `IgnoreSmall` | `0` | 振り仮名らしきものを除外するかどうか |

TVCaption3の`Method`(描画方法)、`FreeType`、`EnOsdCompositor`(映像への字幕合成)、字幕付き画像の保存は、WebView2上のキャンバスへ描画する本プラグインの構造では意味を持たないため対応していません。

テレ東(BSや系列局含)では初回は50秒ほど待たないとデータ放送が表示されません。

## 設定

キー割り当て、パネル、サイドバー、ステータスバーの設定はTVTestの設定で行えます。

### 通信コンテンツ

Plugins/TVTDataBroadcastingWV2.iniを以下のようにすると通信が有効になります。

```ini
[TVTDataBroadcastingWV2]
EnableNetwork=1
```

通信コンテンツの取得ではサーバ証明書を検証します。証明書が不正なサーバに繋がらない場合のみ`IgnoreCertificateErrors=1`で検証を無効にできますが、通信内容を第三者に読み書きされうるため推奨しません。

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

「勢い」パネル(WebView2)内に投稿欄があります。投稿するには「設定」ボタン(ツールチップ「ニコニコログイン」)を押し、「ログイン」ボタンからログインしてください。未ログイン時は投稿欄が無効化されます。

ログインはjkcnslのブラウザーウィンドウで行います。「ログイン」を押すと別ウィンドウが開くので、そこで通常どおりニコニコにサインインし(2段階認証もこのウィンドウ内で完結します)、ウィンドウ内のボタンで完了してください。保存済みのセッションが有効な間はウィンドウは開かず、すぐにログイン済みになります。「ログアウト」を押すとニコニコ側のセッションを破棄し、保存されている認証情報も削除します。

この方式には、ブラウザーログインに対応したjkcnsl(`jkcnsl.exe`と同じ場所に`jkcnsl_login`フォルダーが必要)が要ります。メール/パスワードを直接入力する旧方式のjkcnslには対応していません。
投稿欄の「▷」ボタン(コマンド選択)から色/位置(上/下/流れる)/サイズをNicoJKと同様に選べます。「184」ボタンで匿名投稿を切り替えられます。

### NGフィルタ・置換

コメントログ上で対象行を右クリックすると、そのユーザをNGに追加/解除できます(INI `[NG]`に保存)。正規表現・コマンド単位のNGは`[NG]`を直接編集してください。
`[CustomReplace]`セクションはNicoJKの「chatタグ表示前の置換リスト」と互換です。`Pattern{数値}=s/パターン/置換文字列/g`形式のキーを{数値}の昇順に、コメント本文ではなく`<chat ...>本文</chat>`のタグ全体へ適用し、置換結果を解釈しなおしてから表示・NG判定を行います(過去ログファイルには置換前の原文が残ります)。先頭を大文字`S`にするとそのパターンは無効になります。
置換でNicoJKのローカル拡張属性`abone="1"`(非表示)、`align="left|right"`(上下コメの寄せ)、`insert_at="last"`(直前のコメントの隣に積む)、`yourpost="1"`(背景付きで強調)を付けると表示を変えられます。設定例はINIのコメントを参照してください。

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

サブモジュール（libaribcaptionを含む）を取得し、NuGetパッケージを復元してTVTDataBroadcastingWV2.slnをビルド

```sh
git submodule update --init --recursive
```

新しくクローンする場合は、`git clone --recursive`を使用しても構いません。browser以下のnpmパッケージとlibaribcaptionのサブモジュールは別々に取得します。

Release x64ビルド後、起動時のパネル復元スモークテストは以下で実行できます。

```powershell
.\x64\Release\StartupSmokeTests.exe
```

### web-bmlのビルド

以下のコマンドでビルド

```sh
cd browser
npm ci
npm run build
```

フォントをコピー
```bat
copy browser\node_modules\web-bml-fonts\*.woff2 browser\dist\
```

* Plugins/
    * TVTDataBroadcastingWV2.tvtp
    * TVTDataBroadcastingWV2
        * resources/
            * TVTDataBroadcastingWV2.html
            * dist/
                * TVTDataBroadcastingWV2.js
                * Kosugi-Regular.woff2
                * KosugiMaru-Bold.woff2
                * KosugiMaru-Regular.woff2

のように配置するかTVTDataBroadcastingWV2.tvtpと同じディレクトリにTVTDataBroadcastingWV2.iniを作り以下のようにする

```ini
[TVTDataBroadcastingWV2]
ResourceDirectory=x:\xx\browser\
```
