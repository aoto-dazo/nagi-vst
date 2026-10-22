# Nagi（凪）— resonance suppressor

個人利用のお試し用に一から作った、動的レゾナンス抑制VST3プラグインです。

## 仕組み
STFT（Hann窓・75%オーバーラップ）で周波数ごとに分け、各帯域を周囲の平均（ベースライン）と比べて、突出している共振だけをその瞬間に下げます。

## モード
| モード | FFT | レイテンシー（48kHz） | 用途 |
|---|---|---|---|
| LOW LATENCY | 256 | 約5.3 ms | 歌配信・リアルタイム |
| HIGH QUALITY | 2048 | 約43 ms | ミックス |

96kHzのときはFFTサイズを倍にして、時間あたりの解像度を同じに保ちます。

## パラメータ
- **DEPTH** 最大削減量（dB）
- **SHARPNESS** 高いほど細い共振を狙う
- **SELECTIVITY** 高いほど目立つ共振だけに反応
- **ATTACK / RELEASE** 効き始めと戻りの速さ
- **LOW / HIGH** 処理する周波数範囲（スペクトル上のハンドルでもドラッグ可）
- **TILT** ＋で高域ほど敏感、−で低域ほど敏感
- **MIX / OUTPUT**
- **DELTA** 削られている音だけを聴く

## ビルド
GitHub Actions（`.github/workflows/build.yml`）がWindows用VST3を自動ビルドします。Actionsタブの最新の実行から `Nagi-VST3-Windows-x64` をダウンロードしてください。

手元でビルドする場合:
```
cmake -S . -B build -A x64
cmake --build build --config Release --target Nagi_VST3
```

## インストール
`Nagi.vst3` フォルダを `C:\Program Files\Common Files\VST3\` にコピーし、DAWでプラグインを再スキャンしてください。
