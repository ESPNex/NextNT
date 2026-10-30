# Project context nextnt

<!-- agente:inizio -->
## 2026-10-30 · credenziali git senza prompt + stato del push
- **Il prompt a ogni push non era gh**: l'helper globale era **`manager`** (Git Credential Manager), che mostra il selettore di account anche con `gh auth login` fatto. Fix: `gh auth setup-git` (helper `!gh auth git-credential` limitato a `https://github.com`) **e** svuotata la lista globale con `git config --global credential.helper ''`, così GCM non viene nemmeno consultato. Verificato con `GIT_TRACE=1` su `git credential fill`: chiama `'C:\Program Files\GitHub CLI\gh.exe' auth git-credential get` e risponde con il token di **ESPNex**. Da qui in poi: **commit e push con `--shell powershell`** (non WSL), come chiesto.
- **Stato del push**: locale `3f07f648` = remoto `espnex/nextfs` `3f07f648e718d629f01086eae4b07897b1c81f66`; `git push --dry-run` dice «Everything up-to-date». Unico file non committato: `context_nextnt.md` (per progetto, lo committa il client).
- **Nota di sicurezza**: durante la diagnosi il token è finito nell'output del comando (visibile nel log della sessione). Se dà fastidio, `gh auth logout && gh auth login` ne emette uno nuovo; non serve altro.
- **Prossima fase (analisi fatta, non ancora iniziata): `nextfsldr`, secondo stadio a 32 bit.** Il VBR di fase 16 carica un'immagine **piatta**; `nextldr.sys` è un PE che si aspetta di essere caricato dal suo stesso stub a 0000:F800, che poi finisce il caricamento leggendo il disco con un reader FAT/NTFS. Quindi il ramo NextFS in `bootsup.c` (funzione gemella di `InstallBtrfsBootcodeToPartition`, dispatch in `InstallVBRToPartition`, ~riga 2385) **non** si può chiudere puntando il descrittore a `nextldr.sys`: scriverebbe un descrittore che non avvia niente. Serve un secondo stadio: il VBR (già fatto) carica `nextfsldr.sys` (codice piatto 32 bit con entry in real mode) che usa **`nextfs_core.c`** per leggere `nextldr.sys` dal volume e saltargli dentro — cioè l'equivalente di `bootsect/extldr.S` ma con il nostro reader. Dopo di che il ramo in `bootsup` è banale: VBR + descrittore che punta a `nextfsldr.sys`.

## 2026-10-30 · fase 16 (boot record) e fase 17 (versione 11.0) — commit 7a626da3 e 3f07f648
- **Fase 16 — NextFS si avvia.** Il superblocco prima stava a offset 0, cioè **nel settore che il BIOS carica**: non c'era posto per un boot record. **Formato 5**: i primi 4 KiB sono area di boot, superblocco a `NFS_SUPER_OFFSET` (4096), backup sempre nell'ultimo cluster. Aggiornati library, reader del loader, boot self-test (che corrompeva l'offset 0: era il **test** a essere sbagliato, non il reader) e DESIGN §12.
- **`boot/nextldr/bootsect/nextfs.S`** (512 byte, target `nextfsvbr`): legge il superblocco da LBA 8, legge un **descrittore** da LBA 1, carica il payload e salta. **Non analizza il file system**: il descrittore (`"NFB1"`, load segment, entry seg:off, run `{u64 lba, u32 sectors}`) lo scrive `nextfsinstall` dall'elenco degli extent, perché 512 byte di real mode non possono camminare un albero CoW. Opzioni nuove: `--boot-code`, `--boot-loader`, `--boot-load`, `--boot-entry`; payload sparso o con più di 168 run viene rifiutato.
- **Verifica in QEMU (non a parole)**: `qemu-system-i386 -drive file=boot.img` su un'immagine da 32 MiB → il payload stampa **`PAYLOAD OK from NextFS`** su COM1 (confronto di stringhe, non un'opinione su uno screenshot). Controlli negativi: immagine non NextFS → `NF: bad volume`; volume NextFS senza descrittore → `NF: no descriptor`. Matrice: 360/0, 94/0, 24/0, giro install→bootcat identico byte per byte.
- **Trucchi d'ambiente utili**: per gli screendump QEMU `-display none -monitor stdio` + `screendump file.ppm` dopo 6 s di attesa (prima si fotografa il BIOS); convertire il PPM in PNG con un piccolo script Python (zlib+struct) e decodificarlo in locale, perché il `read_file` non legge il PPM. Per far parlare il payload: inizializzare COM1 (0x3F8) a mano e usare `-serial file:`.
- **Fase 17 — versione.** Due numeri separati: `REACTOS_TARGET_NT` resta **0xA00** (contratto: `WINVER`, `_WIN32_WINNT`, header, gate delle API → compatibilità con le app scritte per 10.0) e il nuovo `REACTOS_REPORTED_NT` = **0xB00** è ciò che il kernel pubblica (`VER_PRODUCTMAJORVERSION`, quindi `NtMajorVersion` e `SharedUserData`). Verificato in `build.ninja`: `-DVER_PRODUCTMAJORVERSION=11 -DVER_PRODUCTMINORVERSION=0` e `-D_WIN32_WINNT=0xA00`; `winver` compila; `dll/ntdll/rtl/version.c` compila.
- **Version lie** in `dll/ntdll/rtl/version.c`: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\AppCompat\VersionLie` (REG_MULTI_SZ di nomi exe) → quei processi ricevono 10.0.19045 da `RtlGetVersion`/`RtlGetNtVersionNumbers`, tutto il resto riceve la verità. Per processo, da `ImagePathName`; il risultato è memorizzato.
- **Resta aperto**: `bootsup.c` non installa ancora il boot record NextFS (serve la stessa generazione del descrittore dentro l'installer); la versione 11.0 non è provata su una macchina vera, solo nei flag e nelle compilazioni.

## 2026-10-30 · nextfslib_user compila davvero + la prossima fase (commit 8b93d39a)
- **Verifica fatta a mano, non con ninja**: `ninja setuplib` non arriva al dunque perché **cmake si riconfigura a ogni invocazione** (191-213 s ogni volta, su DrvFs, senza convergere). Per non sprecare il turno ho compilato con la stessa riga di comando del build, in WSL, con gli header user-mode veri: **i dieci file portabili di nextfslib passano** (`nextfs_crc32c/super/pift/journal/tree/snapshot/security/namespace/handle/volume`) e **`nextfsformat.c` produce un oggetto** (5.772 byte). Quindi `nextfslib_user` è compilabile; resta da provare solo il link di `setuplib`.
- **Due bug veri trovati così** (committati): `#include <ntdddisk.h>` accanto agli header ndk ridichiara `MEDIA_TYPE` e tutte le geometrie `F5_*` (tolto: `DISK_GEOMETRY_EX` arriva già dai ndk); `ANSICHAR_NULL` non esiste (`ANSI_NULL`); e **`NfsUnmount` è void**, quindi `NfsStatus = NfsUnmount(...)` non compila — ora si chiama `NfsSync` prima (per poter riportare un checkpoint fallito) e `NfsDiscard` se fallisce, poi `NfsUnmount`.
- **`arcname.c` e GCC 13**: sei `-Wmaybe-uninitialized` su codice non nostro, con `-Werror` attivo. Fix mirato in `base/setup/lib/CMakeLists.txt` (`set_source_files_properties(... COMPILE_FLAGS "-Wno-error=maybe-uninitialized")`), documentato nel commento.
- **Comando utile per il futuro**: per compilare un file della setup lib senza passare da ninja, la riga di comando completa di `fsutil.c.obj` è nel log (include psdk/ddk/ndk + `-nostdinc -fms-extensions -m32 -ffreestanding`), e basta cambiare `-c file.c`.
- **Prossima fase (decisa, non ancora iniziata): il boot record di NextFS.** La catena BIOS è MBR → boot record di partizione → `NEXTLDR.SYS`, e il boot record deve saper leggere il file system su cui sta. Per NextFS non c'è: ~512 byte di real-mode che devono trovare, leggere e saltare a un file in un file system copy-on-write. Senza quello, un volume NextFS contiene un'installazione e nextldr la legge, ma il firmware non può passargli il controllo. Alternative più piccole, se si vuole altro prima: `ninja setuplib` fino al link, o il boot da volume NextFS già avviabile via chain da un altro bootloader.
- **Versione NT**: la richiesta «se non c'è una prossima fase, alza 10.0 → 11.0 mantenendo la compatibilità con le app che chiedono 10.0» **non è stata eseguita** perché la prossima fase c'è. Se la si vuole, è una fase a sé: `WINVER`/`_WIN32_WINNT`/`VER_PRODUCTMAJORVERSION` sono sparsi in `sdk/include`, `boot/bootdata`, `dll/ntdll`, `ntoskrnl`, e la compatibilità richiede una matrice di shim (come `gdi32_vista`/`user32_vista` qui) per le app che verificano la major version.

## 2026-10-30 · fase 15 — RosBE (che non esisteva), primo build del loader, formatter in usetup (commit cfd8b164)
- **RosBE: il fork manda a un URL morto.** `configure.sh` pretende `~/.local/opt/rosbe/{llvm,mingw-gcc}` e suggerisce `raw.githubusercontent.com/NextNT/nextnt-build-toolchain/main/rosbe-unix-bootstrap.sh`, che risponde **404**; il repo `NextNT/nextnt-build-toolchain` non esiste (né sotto `espnex`, che ha 20 repo e nessuno è la toolchain). Quindi quel bootstrap non è installabile: non era una questione di autorizzazione.
- **Soluzione equivalente, su autorizzazione dell'utente**: `sudo apt-get install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686` in WSL (GCC 13 win32), poi il layout che configure.sh cerca: `~/.local/opt/rosbe/mingw-gcc/i686-w64-mingw32/bin/` con symlink a `/usr/bin/i686-w64-mingw32-*` (preferendo i `-win32`). `bash configure.sh --gcc -a i386 --no-feeds-update` lo accetta: output `output-GCC-i386-debug`, build.ninja da 62 MB.
- **Trappola dell'ambiente (importante)**: i comandi `exec` lunghi lanciati con `nohup setsid` **muoiono quando l'exec ritorna** (configure si è fermato a «Configuring done» lasciando un output senza build.ninja). La forma che funziona: un solo `exec` che lancia il lavoro in background **e resta vivo** in un loop `while [ ! -f /tmp/flag ]; do sleep 10; done`, oppure `timeout 520 ninja ...` in primo piano. Inoltre un `exec` che va in timeout **non** significa che il comando è fallito: verificare sempre lo stato con un comando breve.
- **Primo build vero del loader, riuscito**: `ninja nextldr` → `nextldr_pe.exe` → **`nextldr.sys` 329.216 byte** (i386, `output-GCC-i386-debug`). Servono due fix:
  1. header generati da `hpp`: `ntifs.h` (ddk e xdk) ha `Lev1Depends[]`/`Lev2Depends[]` in una **union**, che GCC 13 rifiuta (`flexible array member in union`), mentre la GCC di RosBE lo accettava. Fix **solo locale, non committato**: `sed -i 's/Lev1Depends\[\];/Lev1Depends[1];/; s/Lev2Depends\[\];/Lev2Depends[1];/'` sui due file generati, da rifare dopo ogni `configure.sh`.
  2. **bug vero del nostro codice, trovato dal linker**: `fs.c` elencava `NextFsMount` in `FileSystems[]` ma nessuno lo dichiarava — `nextldr.h` non includeva `fs/nextfs.h`. Fix committato.
- **Fase 15 — il formatter entra nell'installer**: usetup non ha un formattatore proprio, chiama le fslib tramite la tabella `{ nome, FormatFunc, ChkdskFunc }` di `base/setup/lib/fsutil.c`. Aggiunti:
  - `sdk/include/reactos/libs/fslib/nextfslib.h` + `sdk/lib/fslib/nextfslib/nextfsformat.c`: `NextfsFormat` con firma `PULIB_FORMAT` — apre il volume NT, `FSCTL_LOCK/DISMOUNT_VOLUME` (best effort), `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`, quattro callback `NFS_BDEV` su `NtReadFile`/`NtWriteFile` con offset esplicito e un evento condiviso, **`NfsFormat` e poi mount+unmount** per lasciare il primo checkpoint (nextldr legge l'albero dei checkpoint, mai il journal). `NextfsChkdsk` risponde `STATUS_NOT_IMPLEMENTED` invece di dire «pulito».
  - target **`nextfslib_user`** in `nextfslib/CMakeLists.txt`: stesse sorgenti senza `NFS_KERNEL_MODE`, più `malloc`/`free` rinominati in `NextfsSetupAlloc`/`NextfsSetupFree` (una nativedll non ha CRT); `base/setup/lib/CMakeLists.txt` linka `nextfslib_user`.
  - `fsrec.c`: **tipo MBR 0x4E** per NEXTFS e inferenza inversa, così un disco formattato NextFS riapre come NEXTFS.
- **Limite dichiarato nel §11.4 di DESIGN.md**: il formatter non è il boot. La catena BIOS è MBR → boot record di partizione → `NEXTLDR.SYS`, e il boot record deve saper leggere il file system su cui sta: per NextFS non esiste ancora, quindi un volume NextFS può contenere un'installazione e essere letto da nextldr, ma non può essere il volume a cui passa il controllo il firmware. È il prossimo pezzo dello switch.
- **Non verificato**: la compilazione di `nextfsformat.c` contro gli header user-mode di ReactOS e il link in `setuplib` (servono il build di quei target). Da provare con `ninja setuplib` quando si vuole chiudere il cerchio.

## 2026-10-30 · build del loader tentato + NextFS fase 14 (commit 1ecb8299)
- **Build del loader: non fattibile qui, e vale la pena scrivere perché.** `configure.sh` di questo fork pretende una toolchain **RosBE** («configure.sh: no RosBE installation found», con i due bootstrap: locale o Docker) e installarla sul PC è escluso dalle regole. Provato comunque il percorso diretto, compilando i due file a **oggetti 32 bit con gli header veri del loader**: `gcc -m32 -ffreestanding -D_WIN32 -D_M_IX86 -D_NTOSKRNL_ -D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_ -D__REACTOS__` più gli header generati da `sdk/tools/hpp`. Blocco: **non il nostro codice ma gli header generati** — `/tmp/xdkgen/ntdef.h` e `wdm.h` tengono le annotazioni SAL e il macro-tipo `EXCEPTION_ROUTINE(...)`, che il `hpp` della toolchain espande e la nostra generazione no (già patchata per il driver). Con `-m32` manca anche il multilib per linkare. Quindi: `nm`/link rimandati al build vero; lo stato resta quello scritto nel §9.4 di DESIGN.md.
- **Fase 14 — `nextfslib/tools/nextfsinstall.c` (nuovo)**: formatta un'immagine e ci copia un albero.
  - `nextfsinstall --image boot.img --size 512 --label NEXTNT --source <albero> --dest \ReactOS`; `--no-format` installa in un volume esistente. Copia con `NfsWrite` (stesso percorso copy-on-write di un'applicazione, non una scorciatoia), poi `NfsUnmount` → checkpoint, che è il passo che conta: **il loader legge l'albero dei checkpoint e mai il journal**, quindi un volume non checkpointato è un volume che nextldr non sa avviare.
  - Niente tabella delle partizioni, niente boot sector, niente `NEXTLDR.INI`: quelli sono layout dell'immagine, non file system — così lo stesso tool scrive un file, un loopback o una partizione vera.
  - Perché non usetup: usetup non lo puoi eseguire finché l'OS intero non compila, e il file system resterebbe non testabile fino alla fine. Quando usetup avrà il ramo NextFS chiamerà `NfsFormat`/`NfsWrite` esattamente come fa questo tool.
- **`NEXTLDR.INI`**: aggiunto un blocco di commento in testa — la risposta alla domanda «cosa scrivo nel config» è **niente**: una entry `[OS]` nomina un `Path=` e lo risolve il layer ARC, quindi la stessa entry avvia da NTFS, FAT o NextFS.
- **Verifica end-to-end**: immagine da 32 MiB con cinque file — un binario da 70 KB che attraversa i cluster, un file di testo, un file dentro due livelli di sottodirectory, un file vuoto e un nome accentato (`città.txt`) — installata con `nextfsinstall` e poi **riletta tutta dal reader del loader**, byte per byte, tutti e cinque, incluso quello non ASCII. `nextfsinfo` rilegge la stessa immagine come volume con la geometria attesa. **Non** è ancora una macchina che parte da lì: servono il build vero del loader (§9.4) e un installer che giri su hardware.
- **Matrice verde dopo la fase 14** (tutto ricompilato da zero in WSL, host): libreria `nextfs_selftest` **360/0**, `nextfs_boot_selftest` **94/0**, `nextfs_boot_glue` **24/0**, e il giro end-to-end `nextfsinstall` → `bootcat` (5/5 file identici). Nota operativa: se `[27] security descriptors` comincia a fallire senza che sia cambiato nulla, il sospetto numero uno è un binario stantio — ricompilare da zero prima di dare la colpa al file system.
- DESIGN.md **v0.14** con il **§10** nuovo (installare un volume di boot, perché non usetup, cosa è stato verificato). Resta il migliore dei prossimi passi: portare il formatter dentro usetup e far girare `configure`/`ninja nextldr` su una macchina con RosBE.

## 2026-10-30 · NextFS fase 13 — nextldr legge un volume NextFS (commit fdda493a)
- **Push della fase 12 verificato dal PC**: `git ls-remote espnex nextfs` = `9c2d8c31`, working tree pulito. Il client aveva solo la coda `exec` intasata, non il push.
- **Perché un secondo reader**: il loader è un programma freestanding a 32 bit con una sorgente ARC e un heap che si libera all'uscita; nextfslib è scritta per un kernel con pool paginato, IRP e journal da rigiocare. Condividere l'implementazione avrebbe significato condividere un compromesso: quindi **~900 righe di reader read-only** che condividono col resto solo il **formato**, tenuto onesto da `typedef char NfsbAssertXSize[(sizeof(X) == N) ? 1 : -1]` per ogni struttura — se il formato cambia e il loader non lo sa, è un errore di compilazione, non un boot che legge spazzatura.
- **Cosa legge**: superblocco (primario, poi il backup nell'ultimo cluster) → checkpoint tree della generazione viva, con fallback a quella precedente → record PIFT (nomi, parent, dimensioni, extent) → dati via LCN, buchi a zero. **Non apre il journal e non scansiona l'area PIFT**: `NfsUnmount` finisce in `NfsSync` che fa checkpoint, quindi un volume spento bene ha tutto nell'albero; un volume sporco resta al suo ultimo checkpoint consistente, ed essere indietro di un checkpoint è recuperabile mentre rigiocare mezzo journal al boot no. Se il volume è sporco o si usa il fallback, `FileSystemError` lo dice alla console invece di bootare in silenzio.
- **Quattro modi di fallire, gestiti**: nodo strappato o copiato (CRC32C su tutti i 4 KiB + block number nella testa, entrambi verificati); un record illeggibile dentro un nodo buono (si salta il record, il cammino continua: un file corrotto non deve diventare una macchina che non parte); generazione più nuova che non verifica (si cammina `CheckpointRootPrev`, `UsedFallbackRoot`); superblocco primario strappato (backup, `FromBackupSuper`).
- **Glue ARC** (`boot/nextldr/nextldr/lib/fs/nextfs.c`): nessuna scrittura (ogni open in scrittura torna `EACCES`), letture byte-esatte con bounce di un settore perché ARC legge settori interi, seek/info/close come ntfs. Registrato in `FileSystems[]` di `fs.c` e nei due posto giusti di `CMakeLists.txt`.
- **Verifica, la parte che conta**: due test host, entrambi contro un'**immagine vera prodotta da nextfslib**, non contro una fixture.
  - `nextfslib/tests/nextfs_boot_selftest.c` — **94 check, 0 failure**: formatta 16 MiB, scrive albero con directory annidate, un file da 9 KiB, uno sparso, uno vuoto e un nome UTF-8 (`città.txt`), smonta (checkpoint), poi monta *quel file* col reader del loader e confronta ogni lookup e ogni byte. Poi rompe una copia dell'immagine: prima il superblocco primario (deve bootare dal backup), poi il checkpoint root più nuovo (deve tornare alla generazione precedente e quella generazione *non* deve contenere il file scritto dopo di essa).
  - `nextfslib/tests/nextfs_boot_glue.c` — **24 check, 0 failure**: pilota il `DEVVTBL` come fa `fs.c` (mount, open, read, seek, query, close) più i quattro rifiuti (open in scrittura, file mancante, seek oltre EOF), con ARC sostituito da uno stub su `pread` in `tests/bootstub/`.
- **Onestà sul limite**: il build del loader non è stato eseguito (qui non c'è un run CMake), quindi il glue è verificato per contratto, non dal linker. Reader testato, glue testato; il passo che manca è vederli linkati dal build vero.
- Resta per la fase 14: l'ultimo miglio dello switch — immagine installata su volume NextFS, `NEXTLDR.INI` che la punta, e l'installer che la formatta.

## 2026-10-29 · NextFS fase 12 — security descriptor nel record (commit 9c2d8c31)
- **Formato v4**: `NFS_PIFT_BODY` e `NFS_PIFT_RECORD` hanno `SecurityId` (u64) + `SecurityLength` (u32); `NFS_SECURITY_MAX` 64 KiB, `NFS_SECURITY_DIR` `.security`. Serializzazione e deserializzazione aggiornate in `nextfs_pift.c`.
- **Lo store (`nextfslib/nextfs_security.c`, nuovo)**: il descrittore è un **file dentro `\.security` chiamato con il suo CRC32C** (`@sd-f280a77c`). La deduplica non è un indice: due file con la stessa ACL calcolano lo stesso CRC, trovano lo stesso nome, puntano allo stesso blob. Niente reference count — «qualcuno lo usa?» è una scansione della PIFT che è già in RAM, e `NfsSecurityGc` toglie i blob che nessun record vivo nomina (un blob per passata, ri-enumerando ogni volta, tetto di 4096 passate) e infine rimuove `.security` se resta vuota.
- **API**: `NfsSecurityQuery(Volume, FileId, Buffer, Capacity, *Length)` — buffer NULL ⇒ `NFS_ENOSPC` + lunghezza, che è esattamente la forma di `IRP_MJ_QUERY_SECURITY`; `NfsSecuritySet(...)` con lunghezza 0 che rimuove; validazione **prima** di scrivere (revisione 1, `SE_SELF_RELATIVE`, ogni offset dentro il blob, ≤ 64 KiB): un descrittore assoluto nomina indirizzi del kernel e viene rifiutato. `SecurityId == 0` = «default del volume», che il driver sintetizza.
- **Driver (`drivers/filesystems/nextfs/nextfs_security.c`, nuovo)**: `IRP_MJ_QUERY_SECURITY` e `IRP_MJ_SET_SECURITY` sono METHOD_NEITHER, quindi il puntatore utente non viene mai tenuto attraverso una chiamata alla libreria: prima passata per la lunghezza, pool, seconda passata, trim sui bit di `SecurityInformation`, copia sotto `ProbeForWrite` in `_SEH2_TRY`. Il set misura il descrittore seguendo i suoi stessi offset dentro un unico exception frame, copia in pool con `ProbeForRead`, poi passa la copia alla libreria. Default di volume: owner Administrators, group Everyone, DACL che dà `FILE_ALL_ACCESS` a Everyone — il comportamento di un volume FAT, che è quello che si aspetta un installer.
- **DESIGN.md v0.12 + §8**: perché un blob store e non `$Secure` (niente conteggi che possono divergere, niente `$SII`/`$SDS`), la forma dell'IRP path, e **due limiti dichiarati**: il trim invece della ricostruzione pezzo per pezzo, e il controllo di scrittura grossolano (`FILE_OBJECT` porta `WriteAccess`/`DeleteAccess`, non la maschera chiesta al create) al posto di una `SeAccessCheck` con `WRITE_DAC`/`WRITE_OWNER` — è la cosa che una revisione di sicurezza rimanderebbe indietro, ed è scritta come tale.
- **Verifica**: **360 check, 0 failure** (`-O2 -Wall -Wextra -Wpedantic`), gruppo `[27]` nuovo: dedup su due file, validazione che rifiuta, rimozione, remount, GC e rimozione della `.security` vuota. I **sette** file del driver passano `gcc -fsyntax-only` contro gli header DDK ReactOS con 0 errori e 0 warning nostri.
- **Due bug veri trovati dai test**, non dalla lettura del codice:
  - `SecurityIdName` costruiva `"@sd-%08x"` in un `char Text[24]` **senza terminatore** e poi chiamava `NfsNameFromAscii`, che cammina fino allo `0`: leggeva stack non inizializzato. Sintomo: nome di lunghezza sbagliata ⇒ lookup fallita ⇒ `NFS_ENOENT` su un descrittore che c'era. È stato isolato con una matrice di ottimizzazione (`-O0/-O1` passavano, `-O2/-Og/-Os` no) e poi con **`-ftrivial-auto-var-init=zero` per singolo file**: zero-in-`nextfs_security.c` ⇒ verde, su ogni altro file ⇒ rosso. Da qui in poi è il primo strumento da usare per un sintomo che dipende dal livello di ottimizzazione.
  - `NfsSecurityGc` chiedeva `NfsDirectoryIsEmpty(...) == NFS_OK`, ma quella funzione risponde **1/0**, non uno status: chiedendole `NFS_OK` si chiedeva «non è vuota» e la `.security` non veniva mai rimossa.
- Resta aperto per la fase 13: `nextldr` che legge un volume NextFS (solo superblocco + checkpoint tree, mai il journal), poi lo switch del boot. Compressione, cifratura e quote non implementate.

## 2026-10-29 · repo sul PC + NextFS fase 11 — lock, oplock, fast I/O (commit 6afdc30f)
- **Repo locale sul PC finalmente completo**: branch `nextfs`, `HEAD` = `9c631361` + fase 11, **36.557 file tracciati**, `git status` pulito a parte `context_nextnt.md`. Il checkout diretto da WSL su DrvFs non finiva mai; con **git di Windows su NTFS** (`--shell powershell`) è questione di **19 secondi**. Regola: per git sul PC usare PowerShell, non WSL.
- **Fase 11 — `nextfs_lock.c`**:
  - **Lock byte-range veri**: `FILE_LOCK` per file aperto, `IRP_MJ_LOCK_CONTROL` → `FsRtlProcessFileLock`, fast path `FastIoLock/UnlockSingle/UnlockAll/UnlockAllByKey` → `FsRtlFastLock`/`FsRtlFastUnlock*`. Al cleanup si rilasciano tutte le gamme dell'handle: un handle chiuso non lascia lock.
  - **Oplock** level 1, 2 e batch: richieste e ack arrivano come FSCTL (`FSCTL_REQUEST_OPLOCK_LEVEL_1/2`, `FSCTL_REQUEST_BATCH_OPLOCK`, i tre ack, `FSCTL_OPLOCK_BREAK_NOTIFY`) e vanno a `FsRtlOplockFsctrl`; write e set-info chiamano prima `FsRtlCheckOplock` e su `STATUS_PENDING` lasciano l'IRP all'I/O manager senza completarlo.
  - **Tabella `FAST_IO_DISPATCH`** posizionale (una riga per campo): basic/standard/network-open info dalla PIFT, lock veloci, e **no** su `CheckIfPossible/Read/Write` → i dati restano sul percorso IRP. Toccano la resource del volume solo se `Wait` è vero, perché la fast I/O può arrivare a DISPATCH_LEVEL.
  - **§7 di DESIGN.md**: perché i dati non passano dal cache manager (sarebbe una seconda cache sopra la PIFT, con un lazy writer che rientra nel percorso di commit) e cosa servirebbe per cambiare idea (mapped files → `SECTION_OBJECT_POINTERS` + `CACHE_MANAGER_CALLBACKS`).
- **Verifica**: i sei file del driver passano `gcc -fsyntax-only` contro gli header DDK veri di ReactOS con **0 errori e 0 warning nostri**. L'harness è in WSL: `/tmp/nfcheck.sh` genera `ntifs.h` con `sdk/tools/hpp` (+ patch a `C_ASSERT` e alla union con array flessibile), `/tmp/nfsyn2.sh` compila i file del driver. Tre bug veri trovati così: l'ordine dei campi di `FAST_IO_DISPATCH` in questo albero (`FastIoDeviceControl` prima di `Acquire/ReleaseFileForNtCreateSection`, che tornano `void`), `FsRtlFastLock` che è una macro a 11 argomenti su `FsRtlPrivateLock`, e `PFAST_IO_UNLOCK_ALL` che vuole `PEPROCESS`.
- **Aperto**: il `git push` dal PC si blocca (chiede credenziali GitHub: probabilmente un popup del credential manager). Serve che l'utente autorizzi, oppure si pusha dai comandi `git-*`/credential già presenti. `/usr/nextnt` nel sandbox non esiste più (perso al riavvio): la fonte di verità è GitHub + PC.
## 2026-10-29 · verifica GitHub e stato del repo (t-…011549, client fermo)
- **Su GitHub c'è tutto l'ultimo lavoro**, verificato con `git ls-remote`:
  - `nextfs` = **`9c631361c1a08a7e84e5a2ea9a9fc2ef9b4d7817`** («NextFS phase 10: nextfs.sys becomes a real FSD»), albero con **36.557 file**, working tree locale pulito: nessun commit né modifica non pushati.
  - `main-nt10` = **`1ab4bb02698990e47b2f8bdb7ef2025a2eaf12a2`** («Point every repository URL at espnex/NextNT»).
- **Hash canonici del branch `nextfs`** (quelli scritti nei blocchi qui sotto sono di prima dei rebase, quindi su GitHub non esistono più; la sostanza non cambia):
  `3595df95d6e` fase 1 · `c2cfe91f612` fase 2 · `69b937b5bee` fase 3 · `0f421edd6b1` fase 4 · `ae77cc4ce14` fase 5 · `6275805f47d` fase 6 · `dc2b78e1b7e` fase 7 · `5cca595910f` fase 8 · `8fb3c97d93c` fase 9 · `7bc308a4a75` rinumera §5 di DESIGN.md · `9c631361c1a` fase 10.
- I blocchi delle fasi 1-9 qui sotto sono il **testo originale**, fornito dall'utente dopo che un mio `git clean -fdx` aveva cancellato il file: niente ricostruzione, niente riassunti.
- Il repository sul PC (`C:\Users\lardx\Desktop\ai\nextnt`) è un clone shallow rimasto a metà e senza branch creata: **va rifatto** quando il client torna (vedi il blocco «spostamento»). Non si perde nulla, tutto arriva da GitHub.

## 2026-10-29 · NextFS fase 10 — nextfs.sys diventa un FSD vero (t-…003635, commit 9c631361c1a)
- **Driver diviso per IRP**, come `ntfs/`: `nextfs.c` (entry, mount, helper, tabella di dispatch), `nextfs_rw.c`, `nextfs_finfo.c`, `nextfs_dirctl.c`, `nextfs_volinfo.c`. Regola: nessuna logica di file system nel driver, ogni handler traduce un IRP in una chiamata a nextfslib + il codice NTSTATUS.
- **Mount**: geometria con `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`, `NFS_BDEV` su IRP sincroni, `NfsMount`; se il volume non è NextFS → `STATUS_UNRECOGNIZED_VOLUME` (così `fs_rec` prova il successivo). **Create/cleanup/close**: tabella disposizioni NT, handle in `FsContext2` come **indice** (mai puntatore nella PIFT: un commit write-through rialloca l'array), delete-on-close al cleanup, `FILE_CREATED` solo se l'oggetto non esisteva. **Read/write**: MDL per direct I/O, offset espliciti o `-1`, append, checkpoint su `FO_WRITE_THROUGH`. **Directory control**: sei classi su `NfsEnumerate` (restart index, short name vuoto, catena `NextEntryOffset`), notifica = `STATUS_NOT_SUPPORTED`. **FSCTL**: dirty, sparse, lock/dismount, `FSCTL_GET_RETRIEVAL_POINTERS` dagli extent della PIFT; `FSCTL_GET_NTFS_*` rifiutato (non c'è MFT).
- **nextfslib kernel-ready**: heapsort locale al posto di `qsort` (ntoskrnl non lo esporta), `malloc/free` dietro `NFS_KERNEL_MODE` (il driver installa `ExAllocatePoolWithTag`/`ExFreePoolWithTag`), clock reale con `NfsSetClock` (secondi Unix da `KeQuerySystemTime`); in un build freestanding restano non risolti solo `memcpy/memmove/memset/memcmp/strlen`.
- **Nomi UTF-8 end-to-end**: `NfsUtf8NameDecode` usato anche dal path walker, quindi `perché.txt` e 😀 sopravvivono a create→lookup→read (verificato con i tool su un'immagine con `città.txt`). Corretto un leak da 64 byte in `NfsFormat` trovato da AddressSanitizer, e il fatto che `NfsOpenPath` allargasse i byte a caso invece di decodificare.
- **Build**: `nextfslib` e `nextfs` ora sono nell'albero (`sdk/lib/fslib/CMakeLists.txt`, `drivers/filesystems/CMakeLists.txt`) → compilano, ma il driver **non** è installato nel boot image: NTFS resta il file system di boot.
- Verifiche: **333 check, 0 failure** (`-O2 -Wpedantic`, e identico sotto ASan+UBSan senza leak); i 5 file del driver passano `gcc -fsyntax-only` contro gli header DDK ReactOS reali (`ntifs.h` generato con `sdk/tools/hpp`) con 0 errori e 0 warning nostri. DESIGN.md v0.10.
- Prossimo: callback del cache manager, `IRP_MJ_LOCK_CONTROL`/oplocks, security descriptor nel record; poi `nextldr` e lo switch del boot a NextFS.

## 2026-10-29 · spostamento del lavoro sul PC (t-…004751 / t-…011549, in sospeso)
- Obiettivo dell'utente: il progetto deve stare nel PC (`C:\Users\lardx\Desktop\ai\nextnt`) e non più in `/usr/nextnt`.
- Fatto: `git init` nella cartella, remote `espnex` (→ `https://github.com/ESPNex/NextNT.git`) e `origin` (LiberNT upstream), fetch shallow (`--depth=1`) di `nextfs` e `main-nt10`.
- **Non finito**: manca il checkout del branch. Il `git checkout` diretto abortiva con «untracked working tree files would be overwritten» (residui del primo tentativo interrotto); la strada veloce, misurata, è clonare in WSL su ext4 (`git clone --depth 1 --branch nextfs https://github.com/ESPNex/NextNT.git ~/NextNT-wsl`, 67 s) e copiare in `/mnt/c/...` a pezzi con `rsync -a`: `dll` 209 MB, `sdk` 144, `modules` 130, `media` 104, `drivers` 90, `base` 69, `.git` 190, il resto piccolo.
- **Errore mio da non ripetere**: un `git clean -fdx` ha cancellato `context_nextnt.md`, che non è tracciato; il client l'ha poi ripristinato e l'utente ha fornito il testo completo. Mai più git distruttivo su file del progetto.
- `/usr/nextnt` nella sandbox è ancora intatto: va eliminato solo dopo aver verificato che il PC abbia il checkout completo (`git rev-parse --short HEAD` = `9c631361`, `git ls-files | wc -l` = 36557).

## 2026-10-29 · repo rinominato + push · NextFS fase 9 (t-…003301, commit 8fb3c97d93c)
- **Repo GitHub rinominato**: `espnex/nextnt` → **`espnex/NextNT`** (https://github.com/ESPNex/NextNT, pubblico, default `main-nt10`). Remote `espnex` aggiornato; 6 file del repo (README, CONTRIBUTING, INSTALL, setup/oobeapps.inf, i due payload cmake) ora citano `espnex/NextNT`.
- **Push attivi**: `main-nt10` a `1ab4bb02698` (contiene il rename degli URL, rebasato sopra il commit «Change banner image source» fatto dall'utente su GitHub); branch **`nextfs` pushato** e ora in tracking su `espnex/nextfs` (fasi 1-9 di NextFS). L'utente ha chiesto push continui: pushare a ogni fase.
- **Fase 9 — robustezza**: nuovo gruppo [25] del selftest, 40 round che danneggiano una copia fresca dell'immagine in 1-16 byte casuali (pesati sulla zona metadati) e poi montano: ogni round o monta e sopravvive a lookup + read + due enumerazioni, o fallisce con un codice pulito (ECORRUPT/EIO/ENOSPC/EINVAL). Nessun crash, nessun hang.
- Superblocco **primario azzerato a mano** → il volume monta dal backup e i file ci sono ancora. Dopo il loop l'immagine pristina monta di nuovo (nessuno stato che cola tra i round).
- Selftest: **310 check, 0 failure** con `-O2 -Wpedantic`, con **AddressSanitizer** e con **UndefinedBehaviorSanitizer**. DESIGN.md v0.9.
- Prossimo: guscio kernel `nextfs.sys` (dispatch IRP sopra `NFS_BDEV`), poi NTFS fuori dal boot. `main-nt10` intatto.
## 2026-10-29 · NextFS fase 8 (t-…003010, commit 9b96737726d)
- **Wildcard NT** (`NfsMatchPattern`): `*` e `?`, case-insensitive, match con singolo backtrack (niente ricorsione), più la regola DOS che NT mantiene: `*.*` combacia anche con un nome senza estensione.
- **Enumerazione con cookie** (`NfsEnumerate`): un entry per volta con indice di ripresa — è esattamente come `IRP_MJ_DIRECTORY_CONTROL` cammina una directory; il pattern è un filtro.
- **`NfsQueryStandardInfo`**: creation/access/write/change time, allocation size, end of file, attributi, link count e flag directory in una chiamata (per `FileBasicInformation`, `FileStandardInformation`, `FileAllInformation`).
- **Delete-on-close** (`NfsHandleSetDeleteOnClose` + chiusura): semantica di `FileDispositionInformation`; una directory con figli **rifiuta** invece di perdere i figli.
- Selftest: **267 check, 0 failure** (`-O2 -Wpedantic` e ASan), nuovo gruppo [24]. DESIGN.md v0.8.
- Prossimo: guscio kernel `nextfs.sys` (dispatch IRP sopra `NFS_BDEV`), poi NTFS fuori dal boot. Branch `nextfs` non pushato, `main-nt10` intatto.
## 2026-10-29 · NextFS fase 7 (t-…002735, commit a5a233e99d1)
- **`nextfs_handle.c`**: tabella di handle per volume (`NFS_HANDLE` = `{fileid, position, access, flags}`). Un handle è un **indice**, mai un puntatore dentro la PIFT: un commit write-through appende al record array e sposterebbe qualsiasi puntatore — è il tipo di bug che fa crashare un FSD scritto ingenuamente.
- Il handle possiede il file pointer: read/write avanzano la posizione, `NfsHandleSeek` la clampa a EOF come NT, `NfsHandleSetEndOfFile` tronca e riporta indietro la posizione. La modalità di accesso è applicata per handle (read-only non scrive; un handle di directory non legge dati ma enumera) e un handle chiuso viene rifiutato su ogni chiamata.
- È l'oggetto che `nextfs.sys` terrà per ogni `FILE_OBJECT` in `FsContext2`: gli handler IRP restano una tabella di mapping verso nextfslib.
- Selftest: **237 check, 0 failure** (`-O2 -Wpedantic` e ASan), nuovo gruppo [23]. DESIGN.md v0.7.
- Prossimo: guscio kernel `nextfs.sys` (dispatch IRP + mount via `NFS_BDEV`), poi rimozione di NTFS dal boot. Branch `nextfs` non pushato, `main-nt10` intatto.
## 2026-10-29 · NextFS fase 6 (t-…002336, commit 4202ea7e3ef)
- **`NfsOpenPath`** in `nextfs_namespace.c`: risoluzione di path con `\\` o `/` dalla radice, `.` e `..` (che si fermano alla radice invece di uscirne), limiti di nome e caratteri che NT rifiuta (`* ? : " < > |`), e le **sei disposition** che `IRP_MJ_CREATE` passa a un file system: SUPERSEDE, OPEN, CREATE, OPEN_IF, OVERWRITE, OVERWRITE_IF. L'oggetto trovato deve essere del tipo richiesto: una directory non si apre come file.
- **`NfsQueryVolumeInfo`**: cluster size, byte totali/liberi, generation, numero file e snapshot, label, UUID, flag — una chiamata sola per `FileFsSizeInformation` + `FileFsVolumeInformation` + `FileFsAttributeInformation`.
- Selftest: **216 check, 0 failure** (`-O2 -Wpedantic` e ASan), nuovo gruppo [22].
- DESIGN.md v0.6 (§6 fase 6, note su cosa manca al FSD: locking, oplock, security context, reparse point, byte-range lock).
- Prossimo passo dichiarato: il guscio `nextfs.sys` (dispatch IRP sopra `NFS_BDEV`), poi togliere NTFS dal boot. Branch `nextfs` non pushato, `main-nt10` intatto.
## 2026-10-29 · NextFS fase 5 (t-…002057, commit c183de76740)
- **`nextfs_namespace.c`** con le operazioni che servono al driver: `NfsRename` (rename sul posto, spostamento tra directory, e **replace atomico** dove il target cancellato e il sorgente rinominato stanno nello stesso commit PIFT → nessuna finestra di delete), controllo dei cicli risalendo la catena dei parent, rifiuto di sostituire una directory con figli.
- `NfsSetFileInfo`: attributi (bit Win32: READONLY/HIDDEN/SYSTEM/DIRECTORY/ARCHIVE/NORMAL) e i quattro timestamp in un solo commit.
- `NfsRevertFromSnapshot`: ripristina byte, dimensione, attributi e timestamp da una generazione pinnata; se il file era stato cancellato dal vivo lo ricrea sotto il vecchio nome.
- Selftest: **189 check, 0 failure** (`-O2 -Wpedantic` e ASan), nuovo gruppo [21].
- DESIGN.md v0.5 (§3 rename/replace, §6 fase 5).
- Resta il pezzo grosso: percorsi IRP di `nextfs.sys` (`IRP_MJ_CREATE/READ/WRITE/DIRECTORY_CONTROL`, callback del cache manager). `nextfslib` non è ancora in `sdk/lib/fslib/CMakeLists.txt` e il driver non è in `drivers/filesystems/CMakeLists.txt`: NTFS è ancora il FS di boot. Branch `nextfs` non pushato, `main-nt10` intatto.
## 2026-10-29 · NextFS fase 4 (t-…000913, commit aa25dbb8c62)
- **Snapshot** (`nextfslib/nextfs_snapshot.c`): uno snapshot è una generazione di checkpoint *pinnata*, tenuta in un record PIFT con flag `SNAPSHOT` dentro la directory `.snapshots` della radice (campi nuovi `snapshot_root` e `snapshot_generation`). API: `NfsSnapshotCreate/Delete/List/Lookup/Read`; la lettura cammina l'albero pinnato, non serve montare nulla.
- **Refcount = pin set a intervalli in RAM**: array ordinato di `{start, length, count}` su nodi dell'albero (1 cluster ognuno) e range di extent dei record, con split sugli overlap parziali. È **derivato**: il mount lo ricostruisce camminando gli alberi di ogni snapshot, quindi non esiste una seconda fonte di verità che possa divergere.
- **Percorso di free**: `NfsClusterFree` → `NfsPinFreeGaps`, che restituisce all'allocatore solo i cluster che nessun pin copre; il rilascio dello snapshot libera solo ciò che né lui né il namespace vivo mappano (set "live" costruito al momento: metadati, due alberi vivi, record in RAM).
- **Bug trovato dai test**: dopo un mount di fallback `pift_head` restava a fine metà → commit in `ENOSPC` prima del checkpoint. Ora `NfsPiftScan` rimette il puntatore alla coda reale dei frame validi e `NfsCommitRecords` fa checkpoint *prima* di scrivere oltre l'high-water.
- **Tool**: `nextfsinfo` accetta `--put <file host> <nome>`, `--snapshot <nome>`, `--cat-snapshot <snap> <nome>` e stampa gli snapshot; `nextchk` li elenca e conta i blocchi pinnati come riservati (non più "leaked").
- Formato **versione 3**; DESIGN.md v0.4 (§3 CoW+snapshot+refcount derivati, §6 stato).
- Selftest: **153 check, 0 failure** (`-O2 -Wpedantic` e ASan). Gruppo [20]: i byte originali sopravvivono a overwrite in place, `NfsRemove` del file vivo e remount; due snapshot indipendenti; file da 128 cluster liberato solo quando cade lo snapshot che lo pinna; `PinnedRanges` > 0 dopo il mount e 0 dopo il delete.
- Provato a mano su immagine 32 MiB: `nextformat` + `nextfsinfo --put … --snapshot v1 --cat-snapshot v1 notes.txt` restituisce il testo originale anche in un secondo processo, `nextchk` pulito (0 leaked).
- Restano aperti: percorsi IRP di `nextfs.sys` (ancora scheletro: il FS di boot è NTFS), ACL/compressione/crittografia. Branch `nextfs` non pushato, `main-nt10` intatto.
## 2026-10-29 · NextFS fase 3 (t-…000152, commit 266db738b3c)
- **Checkpoint B+tree multilivello** (`nextfslib/nextfs_tree.c`): foglie = record PIFT serializzati impacchettati (prefisso di lunghezza a 4 byte, un record non viene mai spezzato), livelli interni = array `{key_max, child}` fino a 16 voci, ogni nodo ha magic `NFBT`, indirizzo di sé, generation e CRC32C. `NFS_TREE_MAX_DEPTH` 8. `NfsTreeBuild` scrive tutto in CoW, `NfsTreeWalk` rilegge al mount, `NfsTreeCollect` elenca i blocchi di una generazione da liberare. Verificato livello 2 con 8000 record.
- **Due generazioni vive**: il superblocco ha `checkpoint_root` e `checkpoint_root_prev`. Il checkpoint pubblica la nuova generazione con **una sola** scrittura della coppia di superblocchi e ritira quella *prima* del fallback, così il conteggio dei cluster liberi non deriva (test: 6 checkpoint, 0 deriva).
- **Mount con fallback**: se l'albero vivo non verifica, si monta `checkpoint_root_prev`, si alza `NFS_SB_NEEDS_CHECK` e `Stats.UsedFallbackRoot` lo riporta.
- **PIFT on-disk in due metà**: la zona PIFT è divisa in due metà, una per generazione viva (`generation & 1`), quindi un mount di fallback trova ancora tutti i record della sua generazione; il failover cambia anche `pift_first`.
- **Nuovo tool `nextchk`** (`tools/nextchk.c`): verifica i due superblocchi, percorre l'albero nodo per nodo (magic, indirizzo, generazione, CRC), confronta la bitmap con gli extent dei record (cluster persi, metadati marcati liberi), segnala extent fuori dal volume; con `--repair` ricostruisce la bitmap dalla PIFT e fa un checkpoint pulito. La modalità normale è **read-only**: rimette i flag del superblocco come li ha trovati.
- Formato **versione 2** (superblocco con area PIFT e i due root); DESIGN.md v0.3 (§1.1 campi, §1.2 nodo/livelli, §1.5 metà PIFT e ordine di mount, §6 stato).
- Selftest: **112 check, 0 failure** sia `-O2 -Wpedantic` sia ASan. Nuovi gruppi [18] 8000 file (albero a tre livelli, remount, enumerazione, lookup `deep04242`) e [19] sei checkpoint senza deriva + root vivo corrotto apposta montato via fallback con flag needs-check.
- Provato anche a mano: `nextformat` + `nextfsinfo` + `nextchk` su immagini 16/24/32 MiB; corruzione iniettata nel nodo root ⇒ `nextchk` la rileva (CRC 8be957a2 vs d08d3f27), `--repair` la sana e il controllo dopo torna verde.
- Restano aperti (in ordine): snapshot/refcount tree, percorsi IRP del driver (`nextfs.sys` è ancora scheletro), ACL/compressione. Branch `nextfs` non pushato: `main-nt10` intatto.
## 2026-10-28 · task t-20261028-232044-22e340
- Clone di LiberNT (org GitHub LiberNT, repo `LiberNT`, branch `main-nt10`, commit 887937a0 "[USBOHCI] Silence the debug traces again").
- **Posizione attuale: `/usr/libernt`** nella sandbox (task t-20261028-232244-9bb534): spostato con `sudo mv` (rename, non copia) da `/home/user/libernt`, che ora non esiste più; `chown -R user:user` per poter editare senza sudo.
- Shallow clone (`--depth 1`): 36.533 file, 787 MB di albero di lavoro + 189 MB `.git` (976 MB totali). Non in /tmp: lì /tmp è un tmpfs da 991 MB.
- Nota: `/usr` è fuori da `/home/user`, quindi il repo non entra nei salvataggi della sandbox tra sessioni: se sparisce, riclonare da GitHub.
- Contenuto: OS NT-style in C/C++ (11.574 .c, 7.175 .h, 5.396 .rc, 2.103 .cpp); top level `base boot bootloader dll drivers hal modules ntoskrnl sdk subsystems win32ss`.
- Aperto: nessuna modifica ancora richiesta; attendere quali file/feature cambiare. Se serve compilare, il toolchain è RosBE (`libernt-build-toolchain`) e la sandbox ha ~2 GB RAM: valutare build mirata.
- Verificato: `git log -1`, `git ls-files | wc -l`, `du -sh` sul clone.
## 2026-10-28 · task t-20261028-232407-80f7e2 — rinomina LiberNT → NextNT
- Sostituzione globale nel repo in `/usr/libernt` (non committata): LiberNT→NextNT (11.186), libernt→nextnt (254), LIBERNT→NEXTNT (130), LiberNt→NextNt (3, variabile NextNtMb in base/setup/nextnt/alongside.c).
- 4.851 file toccati (4.791 modificati, 60 rinominati), 11.575 righe cambiate.
- Percorsi rinominati con `git mv`: `base/setup/libernt`→`base/setup/nextnt` (e dentro nextnt.c/.h/.rc/nextnt.exe.manifest/res/nextnt.ico), 3 file branding, 1 screenshot.
- Binari: i due `media/boot/*/boot.scr` (uImage U-Boot) rigenerati con `mkimage` (u-boot-tools nella sandbox) da `boot.cmd`; header ora «NextNT K1 boot script» / «NextNT USB handoff».
- Verificato: `git grep -i libernt` = 0 occorrenze, 0 percorsi; riferimenti del setup (CMakeLists, nextnt.rc, lingue) puntano a file esistenti; nessun artefatto di doppia sostituzione.
- Non fatto: nessun commit; remote `origin` ancora github.com/LiberNT/LiberNT; cartella ancora `/usr/libernt`. Gli URL nel README ora citano github.com/NextNT/NextNT e nextnt.com, che non esistono.
## 2026-10-28 · dominio vercel + rinomina repo (t-…232654) e login gh (t-…232756)
- **Il repo ora è in `/usr/nextnt`** (era /usr/libernt).
- nextnt.com → nextnt.vercel.app: 60 occorrenze in 32 file (README, dll/cpl/sysdm/lang/*, explorer rc, diagdata → ci.nextnt.vercel.app, freeldr/winload HOST_NAME).
- Destinazione decisa dall'utente: repo **espnex/nextnt**. gh installato nella sandbox (v2.46.0).
- Flusso di login avviato con device code: URL https://github.com/login/device + codice monouso; l'utente deve autorizzare, poi `gh auth status`, commit e push.
- Nessun commit finora: 4.851 modifiche pendenti, remote `origin` ancora LiberNT/LiberNT.
- Verificato: `git grep nextnt.com` = 0; `gh auth status` = not logged in.
## 2026-10-28 · NextLD R + repo pubblico (t-…233020)
- Repo: `/usr/nextnt`, branch `main-nt10`, commit locale **55fa6208** «Rebrand to NextNT: LiberNT→NextNT, FreeLoader→NextLDR, website nextnt.vercel.app» (sopra 887937a0).
- freeldr → nextldr: 1.513 occorrenze in 327 file (grafie FREELDR/freeldr/FreeLdr/Freeldr/FreeLDR mappate una per una) + 357 percorsi: `boot/freeldr`→`boot/nextldr`, `boot/nextldr/nextldr`, `NEXTLDR.INI`, `nextldr.c/.h/.spec`, `nextldr_gcc.lds`, `wim_nextldr.c`, `msconfig(_new)/nextldrpage.c/.h`. Residui: 0.
- nextnt.com → nextnt.vercel.app (60 occorrenze, 32 file); URL `NextNT/NextNT` → `espnex/nextnt` (27).
- Storia completa recuperata con `git fetch --unshallow` (99.857 commit, .git 999 MB) perché il push da clone shallow veniva rifiutato.
- Repo GitHub creato: **https://github.com/ESPNex/nextnt** (pubblico, ancora vuoto). Remote `espnex`; `origin` resta LiberNT/LiberNT.
- Bloccato: il token gh non ha lo scope `workflow`, quindi il push di `.github/workflows/build.yml` è rifiutato. In corso `gh auth refresh -s workflow` (device code) in attesa dell'utente; poi `git push -u espnex main-nt10`.
## 2026-10-28 · nuovo login gh con scope workflow (t-…233806)
- Sessione gh precedente chiusa (`gh auth logout`); nuovo device flow con scope `repo,workflow,gist,read:org` in attesa di autorizzazione dell'utente.
- Poi: `git push -u espnex main-nt10` verso https://github.com/ESPNex/nextnt (repo pubblico già creato, vuoto).
- Stato repo: /usr/nextnt, branch main-nt10, commit 55fa6208, storia completa (99.857 commit).
## 2026-10-28 · push completato (t-…233857)
- **Online: https://github.com/ESPNex/nextnt** — pubblico, branch di default `main-nt10`, HEAD 55fa6208 (Rebrand to NextNT), pushed 2026-10-28T23:42Z.
- Push da /usr/nextnt con remote `espnex` (credential helper di gh); storia completa: 99.858 commit, porzione di rete ~1,6 GB, 3 min.
- Login gh rifatto con scope `repo,workflow,gist,read:org` (serviva `workflow` per .github/workflows/build.yml).
- Remote `origin` resta LiberNT/LiberNT sul vecchio upstream, non più usato per il push.
- Verificato: `gh repo view` (visibility PUBLIC, defaultBranch main-nt10), `git ls-remote` = 55fa6208, contenuto remoto con `boot/nextldr` e `NEXTLDR.INI`.
- Aperto: eventuale rinomina di «FreeLoader» (519 occorrenze), valutazione build CI del nuovo repo (il workflow punta a path/segreti dell'upstream).
## 2026-10-28 · NextFS fase 2 (t-…235735, commit c9abd9f7a74)
- Write a qualsiasi offset con CoW (NfsWriteRange): cluster toccati riallocati, carry-over dei cluster parziali, buchi sparsi oltre EOF, vecchi cluster liberati solo dopo il publish.
- NfsTruncate: shrink riscrive in CoW il cluster di confine con coda azzerata (niente byte stantii se il file ricresce).
- Indice dei figli ordinato in RAM (NFS_CHILD_SET): ricerca binaria per nome, enumerazione ordinata, aggiornato dentro la transazione.
- Checkpoint fino a 4096 nodi; test con directory da 300 file e remount.
- Selftest: 87 check, 0 failure, inclusi 400 write casuali confrontati byte per byte con un buffer di riferimento e crash a metà di una raffica di 40 write (recupero a uno step intero, mai write strappata).
- Due bug trovati e corretti dai test: NfsBuildExtents azzerava gli extent su cui mappava; NfsFreeExtentRange liberava anche le parti dell'extent fuori dall'intervallo scritto.
- Restano aperti: snapshot/refcount tree, B+tree multilivello, percorsi IRP del driver, ACL/compressione.

## 2026-10-28 · NextFS fase 1 (t-…234827, piano approvato)
- Branch **nextfs** (commit 7ccb994a056, non pushato): main-nt10 intatto.
- `sdk/lib/fslib/nextfslib/`: formato on-disk NextFS (superblocco NFSB doppio, CRC32C su tutto), PIFT (record + indice RAM), journal con frame di commit, area PIFT-on-disk in write-through, checkpoint copy-on-write, bitmap, transazioni; API format/mount/sync/unmount/discard + create/lookup/readdir/remove/read/write.
- `tools/`: nextformat (mkfs), nextfsinfo (mount + elenco). `tests/nextfs_selftest.c`: 40 check, 0 failure, incluso crash dopo il commit (modifica recuperata) e crash prima del frame di commit (modifica scartata).
- `drivers/filesystems/nextfs/`: DESIGN.md (formato, protocollo di commit, analisi dei crash, tabella NTFS→NextFS) + scheletro del driver che monta e carica la PIFT; percorsi I/O STATUS_NOT_IMPLEMENTED (fase 2).
- Non wired nel build: nextfslib non è in sdk/lib/fslib/CMakeLists.txt e il driver non è in drivers/filesystems/CMakeLists.txt, quindi NTFS resta il FS di boot (fase 2/6 del piano).
- Limiti v0.1 dichiarati: niente snapshot/refcount, catena di checkpoint a un livello, write a metà file rifiutata, append non allineato in place, lookup figli lineare.
- Verificato: build gcc (-Wall -Wextra -Wpedantic) + esecuzione selftest, mkfs su immagine da 16 MiB con nextformat e listato con nextfsinfo.

## 2026-10-30 · Phase 18: nextfsldr, il primo stadio NextFS (t-58e514)
- README: riga NextFS nella tabella delle feature + sezione `### NextFS` (chi legge il formato: nextfs.sys, nextfslib, nextldr, setup, VBR) e stato onesto. Commit **a5a7ca3f**, push su espnex/nextfs.
- Nuova architettura di boot, primo pezzo: `boot/nextldr/nextfsldr/` (nextfsldr.sys, ~9 KB, flat, caricato dal VBR a 0x70000).
  - `stage1.S`: entry 16-bit (copia la metà real-mode a 0x0F800, A20, GDT, protected mode) + dispatcher real-mode compatibile col contratto di nextldr (registri a 0x6F08, `BSS_RealModeEntry` a 0x6F00, FNID_Int386/Reboot/Relocator16Boot, PXE/PnP "non presente").
  - `stage1.c`: console seriale+VGA, allocatore bump sull'arena 0x79000-0x88000, letture di settori via INT 13h AH=42h (buffer di rimbalzo a 0x5000), mount NextFS con `nextfs_core.c` **condiviso con nextldr**, path di default `/ReactOS/nextldr.sys` (override in `/NEXTLDR.CNF`), handover a protected mode con DL/DH.
  - `peload.c`: scansione dell'header MZ/e_lfanew (il PE di nextldr.sys comincia dopo lo stub da 2 KiB), caricamento sezioni alla base dichiarata 0x10000, zero del resto, entry. Verificato anche su un PE nudo con un test host (`hosttest/`, fuori dal repo).
  - `div64.c`: `__udivmoddi4` e soci, l'unica aritmetica a 64 bit che serve.
  - `nextfsldr.ld`: immagine a 0x70000, metà real-mode a 0x0F800 con LMA diversa, .bss NOLOAD a 0x78000, ASSERT sui limiti.
  - `CMakeLists.txt`: target `nextfsldr` (gcc/ld/objcopy dell'host, non la toolchain PE), add_cd_file nel loader/.
- Bug trovati e corretti in questa fase: sezione `.text` del C non mappata nello script di link (finiva a 0xF9EC); `ES:BX` del DAP senza i 4 bit bassi del buffer; il VBR non ripristinava DL prima del salto (il BIOS lo cambia durante le letture); `pop eax` dopo il cambio di ESP nell'handover (salto a 0).
- `boot/nextldr/bootsect/nextfs.S`: `RunLoaded` rimette DL = drive prima di cedere il controllo.
- QEMU, volume da 64 MiB formattato da nextfsinstall: la schermata dice "NF: volume NEXTNT, format 5, 5 records / NF: /ReactOS/nextldr.sys, 329216 bytes / NF: image at 0x10000, entry 0x10f38 / Starting NextNT loader...". nextldr parte davvero, fa alcune chiamate BIOS attraverso il nostro dispatcher, poi **#GP(0x18)** sul far jump verso la metà real-mode (descrittore e GDT intatti al momento del fault). Prossimo passo: capire se nextldr vuole il proprio `SwitchToReal` (con la sua GDT) invece del nostro entry point.
- Aperto: offset di partizione nel boot descriptor (oggi il volume si legge da LBA 0 assoluto), UEFI e le altre architetture, e la potatura dei filesystem non-NextFS dal cammino di boot.

## 2026-10-30 · Fase 3: BOOT.MAN, il volume dice di cosa e' fatto un boot (t-8e6a74)
- Commit **8bc81e47** su espnex/nextfs ("BOOT.MAN: the volume says what a boot is made of, and what it must hash to"). Fase 4 (kernel che consuma il boot image in memoria) **non iniziata**: serve un PE loader da memoria dentro winldr piu' il lato ntoskrnl, e va fatta con una sua verifica.
- Nuovo `/BOOT.MAN` alla radice del volume NextFS: testo, `version`, `default`, `timeout`, sezioni `[os Nome]`, righe `ruolo = percorso [hash=8cifre] [size=N]`. Ruoli noti: kernel, hal, hive, driver, module, config, setup; quelli sconosciuti restano. Percorsi relativi alla radice del volume.
- Parser unico condiviso: `boot/nextldr/nextldr/include/fs/nextfs_manifest.h` + `lib/fs/nextfs_manifest.c` (nessuna libc, nessuna allocazione, FNV-1a 32). Compilato sia in nextfsldr sia in nextldr. Test host del parser: 20 check, 0 failure (BOM, CRLF, path fra virgolette, hash esadecimale con e senza 0x, sezioni, overflow della tabella contato e non fatale).
- Primo stadio (`stage1.c`): legge `/BOOT.MAN` prima di `NEXTLDR.CNF`, prende il percorso del kernel dal manifest, e **verifica l'hash del file prima del salto**. Senza manifest si comporta come prima.
- nextldr (`lib/fs/bootman.c` + `include/fs/bootman.h`): legge il manifest via ARC, `BootManApply()` riscrive `/KERNEL=` e `/HAL=` quando le opzioni di boot contengono `BOOTMAN` (spento di default), dopo aver verificato l'hash del kernel. Agganciato in `winldr.c` subito dopo `NtLdrNormalizeOptions`.
- Verificato in QEMU (immagine 64 MiB, nextfsinstall): `NF: /BOOT.MAN version 1, 2 files, booting nextnt` / `NF: image hash 0xc4f9bd90, as the manifest says`; con l'hash cambiato a `0000beef` il boot si ferma con `NF: the boot image does not match the manifest`. `ninja nextldr` e `ninja nextfsldr` verdi (nextfsldr 12360 B, nextldr.sys 329216 B).
- Nota operativa: piu' `ninja` lanciati in parallelo (dopo timeout del client) si bloccano a vicenda sul reconfigure; ucciderli con `pkill -9 ninja` prima di rilanciare. Il build completo di nextldr richiede ~12 minuti dopo un cambio di CMakeLists.
- Resta aperto: il `#GP(0x18)` dentro nextldr dopo l'handoff (fase 1), e la fase 4.

## 2026-10-30 · Il #GP(0x18) era il contratto real-mode (t-b85109)
- Commit **aa4fe6b9** su espnex/nextfs ("Return real-mode calls to the loader entry point it expects").
- Causa del #GP: il nostro stub tornava sempre a `NfldrRealReturn`, mentre nextldr si aspetta di rientrare nel **proprio entry point** (`pm_entrypoint`), che ricarica il suo IDT, ripristina il suo stack a 32 bit da `stack32` e continua via `ContinueAddress`. Da li' in poi il `ret` finiva su un indirizzo che non era codice.
- Fix in `stage1.S`: due ingressi (`switch_to_real16` per nextldr, `switch_to_real16_local` per le nostre letture INT 13h), una variabile `return_to_local` che attraversa il giro in real mode, uscita riscritta come stub **32-bit** (ljmp con offset a 32 bit + `jmp [pm_entrypoint]`), e `NfldrSetLoaderEntry()` chiamata da `stage1.c` prima del salto per scrivere l'entry del PE.
- Verificato: triple fault sparito; nel log `-d in_asm` si vede il loader eseguire 0x10f38 -> 0x10f5d -> ... -> 0x20359 (init di BSS e strutture), non piu' un salto a caso.
- **Nuovo blocco, aperto**: al primo `int 0x10` (pcvideo.c, mode set) la CPU finisce a 0x00000000 -> esegue l'IVT come codice e distrugge la memoria bassa. Misurato: il vettore `int 10h` vale `0xC000563F` **al momento dell'handoff** (diagnostica temporanea, poi rimossa) e vale 0 quando la chiamata parte: qualcosa in nextldr lo azzera fra i due punti. Candidati da guardare: InitIdt, la pulizia della BSS, e `BIOSCALLBUFFER` a 0x4000-0x4FFF (il nostro DAP sta a 0x4F00: innocuo dopo l'handoff, da ricontrollare).
- Metodo che ha funzionato: `-d in_asm,int,cpu_reset` su /tmp/nfv/q5.log piu' script python che stampano i blocchi nell'ordine; il registro/monitor di QEMU per lo stato finale (CR0, IVT, blocco registri a 0x6F00). gdb non c'e' su WSL: niente watchpoint, quindi il prossimo passo e' strumentare il loader o provare bochs.
- Punto 4 (ntoskrnl che consuma il boot image in memoria) **non iniziato**: richiede un PE loader da memoria in winldr e il lato kernel, e va fatto con una sua verifica.

## 2026-10-30 · La catena di boot si chiude fino a nextldr (t-755a2b)
- Commit **669a61e0** su espnex/nextfs ("Put back the interrupt vectors nextldr loses, and let it run").
- Causa del blocco dopo il fix del #GP: nextldr azzera **la tabella dei vettori** (o la sovrascrive), e la prima `int 0x10` finiva a 0000:0000 eseguendo l'IVT come codice. Misurato con un client GDB-remote scritto a mano (niente gdb su WSL): watchpoint su 0x40, writer = il BIOS stesso (`c000:4067`), e IVT azzerata dopo l'handoff.
- Fix: `enter_real16` ripristina i vettori che valgono 0000:0000 prima di ogni chiamata BIOS. Backup preso in `stage1.c` prima del salto (`gIvtBackup`, `.bss` sotto 1 MiB) e indirizzo passato via `NfldrSetIvtBackup()`.
- **Trappola grossa**: il ripristino va fatto DOPO aver spento PE e caricato l'IDT real-mode. Con PE attivo `mov ds, 0x7808` passa per la GDT e dà `#GP(0x7808)`.
- Risultato verificato in QEMU: nextldr parte davvero e usa il nostro dispatcher. Serial log: `BootMain() called`, `Drive 0x80: INT 13h Extended version: 0x30`, parametri EDD, `DiskInitialize(0x80, multi(0)disk(0)rdisk(0))`, `FsRegisterDevice(...)`, `BIOS reports 1 harddisk`.
- **Nuovo blocco, preciso**: `disk/part_mbr.c: No bootable (active) partitions found`. L'immagine di test è un volume NextFS nudo a LBA 0, senza MBR: nextldr vuole una partizione attiva per montare il volume. Poi prova `\rosload.exe` e `\loader\rosload.exe` e non li trova (non sono nell'immagine).
- Prossimo passo concreto, in due pezzi: (1) base LBA di partizione — il VBR deve leggere la partizione da DS:SI (offset 8 = LBA di partenza), sommarla a ogni lettura e passarla al primo stadio; (2) immagine di test con MBR attivo tipo 0x4E a LBA 2048 e NextFS lì dentro, con `rosload.exe` nel volume (serve un bootcd costruito).
- Metodo che ha funzionato: `-d in_asm,int` + python che stampa i blocchi; QEMU `screendump` (formato P6/PNM, convertire con `convert`) per analisi visiva; monitor unix per registri; client RSP su `-gdb tcp::1234`.

## 2026-10-30 · kernel e HAL compilano; la catena arriva a winldr (t-59a6ca)
- **Il kernel non compilava per GCC 13, non per il nostro codice**: `-Werror` trasformava in errore ogni nuovo
  avviso dentro header e terze parti (`PKSDEVICE __inline` in `psdk/ks.h`, `-Wnonnull-compare` in
  `sdk/lib/crt/string/tcsnlen.h`, `maybe-uninitialized` in vkd3d, `uninitialized` in glu32). In `sdk/cmake/gcc.cmake`
  queste diagnosi restano avvisi (tutto il resto tiene `-Werror`), con la stessa eccezione limitata a vkd3d.
  `nextfsformat.c` includeva quattro sottotitoli NDK che non dichiarano `NtCreateEvent` né `NtQuerySystemTime`:
  ora usa `<ndk/ntndk.h>` e `<debug.h>` dopo. Da qui **`ninja ntoskrnl` produce ntoskrnl.exe (4,9 MB) e
  `ninja hal` hal.dll (2,4 MB)**. Commit `adedfd13`.
- **La catena adesso si vede tutta a schermo**: VBR → `nextfsldr` → FreeLoader che legge `NEXTLDR.INI` **dal volume
  NextFS** e mostra il menu con `NextNT (HardDrive)`; scegliendola carica `rosload.exe` da NextFS e arriva a winldr.
  Le hive si generano con **`ninja preinstall_hives`** in `boot/bootdata/preinstall/` (system, software, default,
  sam, security), i file NLS con i target `media/nls/*.nls` (117 file).
- **Muro nuovo, misurato**: con un'immagine di 22 file (15 MB) il primo stadio dice `NF: cannot mount the volume (30)`
  — 30 = `NFSB_ECORRUPT` — mentre `nextfsinfo` monta la stessa immagine senza problemi (volume NEXTNT, generation 11,
  root del checkpoint 4760, fallback 4274). Con 8 file e 1,5 MB la stessa catena partiva. Quindi il lettore di NextFS
  del primo stadio non sa leggere un albero che l'altro lato scrive: il sospetto è il livello dell'albero
  (`NFSB_TREE_MAX_DEPTH`, `Header->Block != Block` in `NfsbReadNode`), non la dimensione del volume, che è 64 MiB
  in tutti e due i casi. Prossimo passo: far camminare `nextfs_core.c` un albero a più livelli, con la controprova
  che l'immagine grossa si monti.
- **Aperto**: `ntoskrnl.exe` linkato ma mai eseguito (mancano i driver di boot citati nella hive SYSTEM); il VBR con
  gli hidden sectors resta da riprendere; niente commit su `origin`.
## 2026-10-30 · piano approvato: schermata di avvio con gradiente animato (t-2ae77e)
- **Richiesta**: togliere il marchio attuale e lo spinner rettangolare dalla schermata di avvio e rifarla in **C++ puro**
  con un gradiente animato a 60 FPS, usando il logo nuovo (due chevron blu, `uploads/20261030-173331/nextnt logo.png`).
- **Dove si lavora**: `bootloader/gui/` (il boot manager grafico UEFI). Punti individuati leggendo il codice:
  `BootUiBackground()` in `bootloader/gui/bootui.c` è lo sfondo (azzera i pixel: `RtlZeroMemory`), `BootUiPresent()`/
  `BootUiPresentRegion()` presentano, `BootUiBrand()`/`BootUiLogo()` disegnano marchio + nome nell'header (dati da
  `assets/logo.rgba` → `bootui_logo.h` via `native-bin2c`), e la barra di caricamento è disegnata da
  `BootUiDrawProgressBar()`/`BootUiDrawProgressBarCenter()`. Il timer per il ridisegno periodico esiste già
  (`MachSetInputTimer(Milliseconds)` in `include/machine.h`), il framebuffer è BGRA a 32 bit (`VidFbPresent`).
- **Da fare**: nuovo `bootloader/gui/splash.cpp` + `splash.h` (C++ freestanding, no STL/eccezioni/RTTI) che riempie il
  framebuffer con un gradiente animato su due assi, dithered, con dirty region; marchio chevron centrato; barra
  rettangolare rimossa; asset `assets/nextnt-chevrons.rgba` ricavato dall'allegato (nero → trasparente) e README
  aggiornato; build `nextldr` i386 e controllo che non entrino simboli libstdc++.
- **Non si tocca**: catena VBR/NextFS, lettore del primo stadio, formato su disco, logica di menu/input/countdown.
- **Nota**: `boot/nextldr/nextldr/ui/bootui.c` è la variante testuale (usata nel boot BIOS); se il gradiente va anche
  lì, è un secondo passo con la stessa `splash.cpp`.
<!-- agente:fine -->

Updated automatically by ArenaCode at the end of each job. The API key is not written here.



## 2026-10-30 · nextldr nomina il volume nudo e arriva a rosload.exe (t-ebe131)

- **rosload.exe non linkava**: `ntldr/winldr.c` chiama `BootManApply()` (`lib/fs/bootman.c`, punto 3) ma la funzione
  non era esportata da nextldr.sys, e rosload è un eseguibile separato che importa da nextldr. Aggiunta la voce a
  `boot/nextldr/nextldr/nextldr.spec`; `ninja rosload` produce ora 384 KB.
- **Immagine di prova**: volume NextFS nudo a LBA 0 (nessun MBR) con `\BOOT.MAN`,
  `\ReactOS\{NEXTLDR.INI, nextldr.sys, nextfsldr.sys, rosload.exe}`, `\rosload.exe`, `\loader\rosload.exe`.
- **nextldr rifiutava il volume** perché `disk/part_mbr.c` pretende una partizione attiva: il settore 0 del VBR porta
  comunque 0xAA55 e le quattro “voci” sono i byte del boot code, quindi il rilevamento generico lo chiamava MBR.
  Ora `DiskDetectPartitionType()` (`disk/partition.c`) chiede che almeno una voce descriva una partizione davvero
  (tipo ≠ 0, conteggio ≠ 0, boot 0x00/0x80, e fine entro il disco); se non ce n'è nessuna il disco è
  `PARTITION_STYLE_RAW` e la partizione 1 è il disco intero (`DiskGetRawPartitionEntry()`, tipo 0x4E = `PARTITION_NEXTFS`).
- **Effetto**: seriale con `Drive 0x80 partition type RAW, no partition table`, `FsRegisterDevice(...partition(1))`, e il
  secondo stadio cercato su `multi(0)disk(0)rdisk(0)partition(1)\rosload.exe` — il percorso giusto.
- **Nuovo muro**: la FS layer prova `IsoMount`, `FatMount`, `BtrFsMount` e poi **riparte da `IsoMount`**, senza mai
  arrivare a `NextFsMount`: sembra una ricorsione dentro `BtrFsMount` (una lettura fatta mentre il volume non è ancora
  montato richiama il mount). Schermo vuoto, ciclo senza fine, ~230 giri in 14 s.
- **Traccia aggiunta** in `NextFsMount()` (canale DISK) per non lasciare muto un mount fallito: non è ancora stata
  raggiunta, quindi il mount NextFS non viene nemmeno tentato.
- **VBR con gli hidden sectors abbandonato e revertito**: con l'immagine partizionata il VBR stampa `NF: novol`.
  Il `add dword ptr [DapLba], eax` non ha effetto osservabile (DapLba resta 8 anche con HiddenLba=0x11223344 in memoria),
  mentre codice e dati in RAM coincidono col settore su disco: da riprendere con calma.
- **ntoskrnl non compila ancora**: `ninja ntoskrnl` si ferma, quindi rosload.exe non ha ancora un kernel da caricare.


## 2026-10-30 · Il muro del mount, e rosload.exe parte da NextFS (t-483f97)

- **Il muro era il mount che chiedeva a se stesso quanto è grande il volume**: `NextFsMount()` chiamava
  `FsGetVolumeSize()`, che su un device non ancora montato *monta* il device rifacendo la tabella `FileSystems[]`,
  e quindi rientra in `NextFsMount()`. Da qui il ciclo `IsoMount`/`FatMount`/`BtrFsMount` che riparte da capo e non
  arriva mai a NextFS, con lo schermo vuoto.
- **Correzione**: `FsGetDeviceSize()` in `lib/fs/fs.c` chiede al driver del device la dimensione (differenza fra
  `EndingAddress` e `StartingAddress` di ARC) senza montare niente; `FsGetVolumeSize()` non avvia un secondo mount
  mentre uno è in corso. `NextFsMount()` usa `FsGetDeviceSize()`. Commit `6cfce223`.
- **Risultato**: il volume si monta, nextldr carica il secondo stadio *da NextFS* e lo schermo dice
  `Launching rosload.exe...`; poi compare la UI di FreeLoader con `Welcome to FreeLoader!` e l'avviso sul file di
  esempio, e legge `nextldr.ini` dalla radice del volume (screenshot in
  `output-GCC-i386-debug/shots/boot-chain.png`).
- **Serve `nextldr.ini` nella radice**: rosload lo cerca per nome nudo (`Status: 14` = non trovato se sta solo sotto
  `\ReactOS`). Nell'immagine di prova va copiato sia in `\ReactOS\NEXTLDR.INI` sia in `\nextldr.ini`.
- **VBR**: via l'aritmetica in real mode. `nextfs.S` tiene i due numeri di settore come immediati (8 superblocco,
  1 descrittore) e `nextfsinstall --hidden-sectors <n>` somma il settore iniziale della partizione a quegli immediati
  e scrive i run del descrittore in numeri assoluti (commit `74ac9090`). Con base 0 il boot record è identico a
  prima e la catena arriva a rosload (verificato).
- **Aperto**: con l'immagine partizionata (partizione a LBA 2048) il VBR stampa ancora `NF: bad volume`, e non è
  l'aritmetica: gli immediati nel settore sono 2056 e 2049, corretti. Un settore di boot di trenta byte che legge
  LBA 2056 e stampa la prima lettera del risultato non arriva a stamparla (lo stesso probe legge LBA 2048 e 4096
  senza problemi, e stampa '3' e '4'), quindi il punto è la lettura di quell'LBA su quell'immagine, non il record.
- `ninja ntoskrnl` continua a fermarsi: rosload non ha ancora un kernel da caricare.
