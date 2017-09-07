# Improved PRAMFS   
(Improved Protected and Persistent RAM Filesystem)   


## Summary   
PRAMFS is a high-speed non-volatile memory system.   
I made improvements on that file system.   

### pramfs-original
The original PRAMFS.   

### pramfs-fixes
Bug improved version.   

### pramfs-dp
PRAMFS for [dp-lkernel](https://github.com/kohga/dp-lkernel).   

### pramfs-jbd
PRAMFS for [nvm-lkernel](https://github.com/kohga/nvm-lkernel).   
This file system implements a journaling system.   


## Original File System   
- [pramfs](http://pramfs.sourceforge.net)   


## Reference  
- PRAMFSにおける誤書き込みからのデータ保護機能, 電子情報通信学会総合大会講演論文集 2016年_情報システム(1), 55, 2016-03-01, 一般社団法人電子情報通信学会   
>- <http://www.gakkai-web.net/gakkai/ieice/G_2016/Settings/ab/d_06_001.html>   
>- <http://ci.nii.ac.jp/naid/110010036689>   

- 高速不揮発性メモリ向けMmapにおける障害に対してアトミックな同期方式   
>- <http://www.gakkai-web.net/gakkai/ieice/G_2017/Settings/ab/d_06_001.html>   
