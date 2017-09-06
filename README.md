# Improved PRAMFS   
(Improved Protected and Persistent RAM Filesystem)   


## Summary   
高速不揮発性メモリ向けファイルシステムのPRAMFS改良版 (※現在開発途中)   
元のPRAMFSのバグは修正済み。   

※ 今後はpramfsにジャーナリングシステムを実装予定   
　（ジャーナリングはJBDを用いる）   

## Base File System   
- [pramfs](http://pramfs.sourceforge.net)   



## Reference  
- PRAMFSにおける誤書き込みからのデータ保護機能, 電子情報通信学会総合大会講演論文集 2016年_情報システム(1), 55, 2016-03-01, 一般社団法人電子情報通信学会   
>- <http://www.gakkai-web.net/gakkai/ieice/G_2016/Settings/ab/d_06_001.html>   
>- <http://ci.nii.ac.jp/naid/110010036689>   

- 高速不揮発性メモリ向けMmapにおける障害に対してアトミックな同期方式   
>- <http://www.gakkai-web.net/gakkai/ieice/G_2017/Settings/ab/d_06_001.html>   
