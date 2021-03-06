; -----------------------------------------------------------------------
;
;   Copyright 1994-2009 H. Peter Anvin - All Rights Reserved
;   Copyright 2009 Intel Corporation; author: H. Peter Anvin
;
;   This program is free software; you can redistribute it and/or modify
;   it under the terms of the GNU General Public License as published by
;   the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
;   Boston MA 02110-1301, USA; either version 2 of the License, or
;   (at your option) any later version; incorporated herein by reference.
;
; -----------------------------------------------------------------------

;
; diskstart.inc
;
; Common early-bootstrap code for harddisk-based Syslinux derivatives.
;

		; Expanded superblock
		section .bss
		alignb 8
SuperInfo	resq 16			; The first 16 bytes expanded 8 times

		section .text
;
; Some of the things that have to be saved very early are saved
; "close" to the initial stack pointer offset, in order to
; reduce the code size...
;
StackBuf	equ STACK_TOP-44-32	; Start the stack here (grow down - 4K)
PartInfo	equ StackBuf		; Saved partition table entry
FloppyTable	equ PartInfo+16		; Floppy info table (must follow PartInfo)
OrigFDCTabPtr	equ StackBuf-8		; The 2nd high dword on the stack
OrigESDI	equ StackBuf-4		; The high dword on the stack

;
; Primary entry point.  Tempting as though it may be, we can't put the
; initial "cli" here; the jmp opcode in the first byte is part of the
; "magic number" (using the term very loosely) for the DOS superblock.
;
bootsec		equ $
_start:		jmp short start		; 2 bytes
		nop			; 1 byte
;
; "Superblock" follows -- it's in the boot sector, so it's already
; loaded and ready for us
;
bsOemName	db MY_NAME		; The SYS command sets this, so...
		zb 8-($-bsOemName)

;
; These are the fields we actually care about.  We end up expanding them
; all to dword size early in the code, so generate labels for both
; the expanded and unexpanded versions.
;
%macro		superb 1
bx %+ %1	equ SuperInfo+($-superblock)*8+4
bs %+ %1	equ $
		zb 1
%endmacro
%macro		superw 1
bx %+ %1	equ SuperInfo+($-superblock)*8
bs %+ %1	equ $
		zw 1
%endmacro
%macro		superd 1
bx %+ %1	equ $			; no expansion for dwords
bs %+ %1	equ $
		zd 1
%endmacro
superblock	equ $
		superw BytesPerSec
		superb SecPerClust
		superw ResSectors
		superb FATs
		superw RootDirEnts
		superw Sectors
		superb Media
		superw FATsecs
		superw SecPerTrack
		superw Heads
superinfo_size	equ ($-superblock)-1	; How much to expand
		superd Hidden
		superd HugeSectors
		;
		; This is as far as FAT12/16 and FAT32 are consistent
		;
		; FAT12/16 need 26 more bytes,
		; FAT32 need 54 more bytes
		;
superblock_len_fat16	equ $-superblock+26
superblock_len_fat32	equ $-superblock+54
		zb 54			; Maximum needed size
superblock_max	equ $-superblock

SecPerClust	equ bxSecPerClust
;
; Note we don't check the constraints above now; we did that at install
; time (we hope!)
;
start:
		cli			; No interrupts yet, please
		cld			; Copy upwards
;
; Set up the stack
;
		xor ax,ax
		mov ss,ax
		mov sp,StackBuf		; Just below BSS
		push es			; Save initial ES:DI -> $PnP pointer
		push di
		mov es,ax
;
; DS:SI may contain a partition table entry.  Preserve it for us.
;
		mov cx,8		; Save partition info
		mov di,PartInfo
		rep movsw

		mov ds,ax		; Now we can initialize DS...

;
; Now sautee the BIOS floppy info block to that it will support decent-
; size transfers; the floppy block is 11 bytes and is stored in the
; INT 1Eh vector (brilliant waste of resources, eh?)
;
; Of course, if BIOSes had been properly programmed, we wouldn't have
; had to waste precious space with this code.
;
		mov bx,fdctab
		lfs si,[bx]		; FS:SI -> original fdctab
		push fs			; Save on stack in case we need to bail
		push si

		; Save the old fdctab even if hard disk so the stack layout
		; is the same.  The instructions above do not change the flags
		mov [DriveNumber],dl	; Save drive number in DL
		and dl,dl		; If floppy disk (00-7F), assume no
					; partition table
		js harddisk

floppy:
		mov cl,6		; 12 bytes (CX == 0)
		; es:di -> FloppyTable already
		; This should be safe to do now, interrupts are off...
		mov [bx],di		; FloppyTable
		mov [bx+2],ax		; Segment 0
		fs rep movsw		; Faster to move words
		mov cl,[bsSecPerTrack]  ; Patch the sector count
		mov [di-8],cl
		; AX == 0 here
		int 13h			; Some BIOSes need this

		jmp short not_harddisk
;
; The drive number and possibly partition information was passed to us
; by the BIOS or previous boot loader (MBR).  Current "best practice" is to
; trust that rather than what the superblock contains.
;
; Would it be better to zero out bsHidden if we don't have a partition table?
;
; Note: di points to beyond the end of PartInfo
;
harddisk:
		test byte [di-16],7Fh	; Sanity check: "active flag" should
		jnz no_partition	; be 00 or 80
		mov eax,[di-8]		; Partition offset (dword)
		mov [bsHidden],eax
no_partition:
;
; Get disk drive parameters (don't trust the superblock.)  Don't do this for
; floppy drives -- INT 13:08 on floppy drives will (may?) return info about
; what the *drive* supports, not about the *media*.  Fortunately floppy disks
; tend to have a fixed, well-defined geometry which is stored in the superblock.
;
		; DL == drive # still
		mov ah,08h
		int 13h
		jc no_driveparm
		and ah,ah
		jnz no_driveparm
		shr dx,8
		inc dx			; Contains # of heads - 1
		mov [bsHeads],dx
		and cx,3fh
		mov [bsSecPerTrack],cx
no_driveparm:
not_harddisk:
;
; Ready to enable interrupts, captain
;
		sti

;
; Do we have EBIOS (EDD)?
;
eddcheck:
		mov bx,55AAh
		mov ah,41h		; EDD existence query
		mov dl,[DriveNumber]
		int 13h
		jc .noedd
		cmp bx,0AA55h
		jne .noedd
		test cl,1		; Extended disk access functionality set
		jz .noedd
		;
		; We have EDD support...
		;
		mov byte [getlinsec.jmp+1],(getlinsec_ebios-(getlinsec.jmp+2))
.noedd:

;
; Load the first sector of LDLINUX.SYS; this used to be all proper
; with parsing the superblock and root directory; it doesn't fit
; together with EBIOS support, unfortunately.
;
		mov eax,[FirstSector]	; Sector start
		mov bx,ldlinux_sys	; Where to load it
		call getonesec

		; Some modicum of integrity checking
		cmp dword [ldlinux_magic+4],LDLINUX_MAGIC^HEXDATE
		jne kaboom

		; Go for it...
		jmp ldlinux_ent

;
; getonesec: get one disk sector
;
getonesec:
		mov bp,1		; One sector
		; Fall through

;
; getlinsec: load a sequence of BP floppy sector given by the linear sector
;	     number in EAX into the buffer at ES:BX.  We try to optimize
;	     by loading up to a whole track at a time, but the user
;	     is responsible for not crossing a 64K boundary.
;	     (Yes, BP is weird for a count, but it was available...)
;
;	     On return, BX points to the first byte after the transferred
;	     block.
;
;            This routine assumes CS == DS, and trashes most registers.
;
; Stylistic note: use "xchg" instead of "mov" when the source is a register
; that is dead from that point; this saves space.  However, please keep
; the order to dst,src to keep things sane.
;
getlinsec:
		add eax,[bsHidden]		; Add partition offset
		xor edx,edx			; Zero-extend LBA (eventually allow 64 bits)

.jmp:		jmp strict short getlinsec_cbios

;
; getlinsec_ebios:
;
; getlinsec implementation for EBIOS (EDD)
;
getlinsec_ebios:
.loop:
                push bp                         ; Sectors left
.retry2:
		call maxtrans			; Enforce maximum transfer size
		movzx edi,bp			; Sectors we are about to read
		mov cx,retry_count
.retry:

		; Form DAPA on stack
		push edx
		push eax
		push es
		push bx
		push di
		push word 16
		mov si,sp
		pushad
                mov dl,[DriveNumber]
		push ds
		push ss
		pop ds				; DS <- SS
                mov ah,42h                      ; Extended Read
		int 13h
		pop ds
		popad
		lea sp,[si+16]			; Remove DAPA
		jc .error
		pop bp
		add eax,edi			; Advance sector pointer
		sub bp,di			; Sectors left
                shl di,SECTOR_SHIFT		; 512-byte sectors
                add bx,di			; Advance buffer pointer
                and bp,bp
                jnz .loop

                ret

.error:
		; Some systems seem to get "stuck" in an error state when
		; using EBIOS.  Doesn't happen when using CBIOS, which is
		; good, since some other systems get timeout failures
		; waiting for the floppy disk to spin up.

		pushad				; Try resetting the device
		xor ax,ax
		mov dl,[DriveNumber]
		int 13h
		popad
		loop .retry			; CX-- and jump if not zero

		;shr word [MaxTransfer],1	; Reduce the transfer size
		;jnz .retry2

		; Total failure.  Try falling back to CBIOS.
		mov byte [getlinsec.jmp+1],(getlinsec_cbios-(getlinsec.jmp+2))
		;mov byte [MaxTransfer],63	; Max possibe CBIOS transfer

		pop bp
		; ... fall through ...

;
; getlinsec_cbios:
;
; getlinsec implementation for legacy CBIOS
;
getlinsec_cbios:
.loop:
		push edx
		push eax
		push bp
		push bx

		movzx esi,word [bsSecPerTrack]
		movzx edi,word [bsHeads]
		;
		; Dividing by sectors to get (track,sector): we may have
		; up to 2^18 tracks, so we need to use 32-bit arithmetric.
		;
		div esi
		xor cx,cx
		xchg cx,dx		; CX <- sector index (0-based)
					; EDX <- 0
		; eax = track #
		div edi			; Convert track to head/cyl

		; We should test this, but it doesn't fit...
		; cmp eax,1023
		; ja .error

		;
		; Now we have AX = cyl, DX = head, CX = sector (0-based),
		; BP = sectors to transfer, SI = bsSecPerTrack,
		; ES:BX = data target
		;

		call maxtrans			; Enforce maximum transfer size

		; Must not cross track boundaries, so BP <= SI-CX
		sub si,cx
		cmp bp,si
		jna .bp_ok
		mov bp,si
.bp_ok:

		shl ah,6		; Because IBM was STOOPID
					; and thought 8 bits were enough
					; then thought 10 bits were enough...
		inc cx			; Sector numbers are 1-based, sigh
		or cl,ah
		mov ch,al
		mov dh,dl
		mov dl,[DriveNumber]
		xchg ax,bp		; Sector to transfer count
		mov ah,02h		; Read sectors
		mov bp,retry_count
.retry:
		pushad
		int 13h
		popad
		jc .error
.resume:
		movzx ecx,al		; ECX <- sectors transferred
		shl ax,SECTOR_SHIFT	; Convert sectors in AL to bytes in AX
		pop bx
		add bx,ax
		pop bp
		pop eax
		pop edx
		add eax,ecx
		sub bp,cx
		jnz .loop
		ret

.error:
		dec bp
		jnz .retry

		xchg ax,bp		; Sectors transferred <- 0
		shr word [MaxTransfer],1
		jnz .resume
		; Fall through to disk_error

;
; kaboom: write a message and bail out.
;
disk_error:
kaboom:
		xor si,si
		mov ss,si
		mov sp,StackBuf-4	; Reset stack
		mov ds,si		; Reset data segment
		pop dword [fdctab]	; Restore FDC table
.patch:					; When we have full code, intercept here
		mov si,bailmsg

		; Write error message, this assumes screen page 0
.loop:		lodsb
		and al,al
                jz .done
		mov ah,0Eh		; Write to screen as TTY
		mov bx,0007h		; Attribute
		int 10h
		jmp short .loop
.done:
		cbw			; AH <- 0
.again:		int 16h			; Wait for keypress
					; NB: replaced by int 18h if
					; chosen at install time..
		int 19h			; And try once more to boot...
.norge:		jmp short .norge	; If int 19h returned; this is the end

;
; Truncate BP to MaxTransfer
;
maxtrans:
		cmp bp,[MaxTransfer]
		jna .ok
		mov bp,[MaxTransfer]
.ok:		ret

;
; Error message on failure
;
bailmsg:	db 'Boot error', 0Dh, 0Ah, 0

		; This fails if the boot sector overflows
		zb 1F8h-($-$$)

FirstSector	dd 0xDEADBEEF			; Location of sector 1
MaxTransfer	dw 0x007F			; Max transfer size

; This field will be filled in 0xAA55 by the installer, but we abuse it
; to house a pointer to the INT 16h instruction at
; kaboom.again, which gets patched to INT 18h in RAID mode.
bootsignature	dw kaboom.again-bootsec

;
; ===========================================================================
;  End of boot sector
; ===========================================================================
;  Start of LDLINUX.SYS
; ===========================================================================

ldlinux_sys:

syslinux_banner	db CR, LF, MY_NAME, ' ', VERSION_STR, ' ', DATE_STR, ' ', 0
		db CR, LF, 1Ah	; EOF if we "type" this in DOS

		alignz 8
ldlinux_magic	dd LDLINUX_MAGIC
		dd LDLINUX_MAGIC^HEXDATE

;
; This area is patched by the installer.  It is found by looking for
; LDLINUX_MAGIC, plus 8 bytes.
;
patch_area:
LDLDwords	dw 0		; Total dwords starting at ldlinux_sys,
				; not including ADVs
LDLSectors	dw 0		; Number of sectors - (bootsec+this sec)
				; but including any ADVs
CheckSum	dd 0		; Checksum starting at ldlinux_sys
				; value = LDLINUX_MAGIC - [sum of dwords]
%if IS_EXTLINUX
CurrentDir	dd 2		; "Current" directory inode number
%endif

; Space for up to 64 sectors, the theoretical maximum
SectorPtrs	times 64 dd 0

ldlinux_ent:
;
; Note that some BIOSes are buggy and run the boot sector at 07C0:0000
; instead of 0000:7C00 and the like.  We don't want to add anything
; more to the boot sector, so it is written to not assume a fixed
; value in CS, but we don't want to deal with that anymore from now
; on.
;
		jmp 0:.next
.next:
		sti		; In case of broken INT 13h BIOSes

;
; Tell the user we got this far
;
		mov si,syslinux_banner
		call writestr_early

;
; Tell the user if we're using EBIOS or CBIOS
;
print_bios:
		mov si,cbios_name
		cmp byte [getlinsec.jmp+1],(getlinsec_ebios-(getlinsec.jmp+2))
		jne .cbios
		mov si,ebios_name
.cbios:
		mov [BIOSName],si
		call writestr_early

		section .bss
%define	HAVE_BIOSNAME 1
BIOSName	resw 1

		section .text
;
; Now we read the rest of LDLINUX.SYS.	Don't bother loading the first
; sector again, though.
;
load_rest:
		mov si,SectorPtrs
		mov bx,7C00h+2*SECTOR_SIZE	; Where we start loading
		mov cx,[LDLSectors]

.get_chunk:
		jcxz .done
		xor bp,bp
		lodsd				; First sector of this chunk

		mov edx,eax

.make_chunk:
		inc bp
		dec cx
		jz .chunk_ready
		inc edx				; Next linear sector
		cmp [si],edx			; Does it match
		jnz .chunk_ready		; If not, this is it
		add si,4			; If so, add sector to chunk
		jmp short .make_chunk

.chunk_ready:
		call getlinsecsr
		shl bp,SECTOR_SHIFT
		add bx,bp
		jmp .get_chunk

.done:

;
; All loaded up, verify that we got what we needed.
; Note: the checksum field is embedded in the checksum region, so
; by the time we get to the end it should all cancel out.
;
verify_checksum:
		mov si,ldlinux_sys
		mov cx,[LDLDwords]
		mov edx,-LDLINUX_MAGIC
.checksum:
		lodsd
		add edx,eax
		loop .checksum

		and edx,edx			; Should be zero
		jz all_read			; We're cool, go for it!

;
; Uh-oh, something went bad...
;
		mov si,checksumerr_msg
		call writestr_early
		jmp kaboom

;
; -----------------------------------------------------------------------------
; Subroutines that have to be in the first sector
; -----------------------------------------------------------------------------

;
;
; writestr_early: write a null-terminated string to the console
;	    This assumes we're on page 0.  This is only used for early
;           messages, so it should be OK.
;
writestr_early:
		pushad
.loop:		lodsb
		and al,al
                jz .return
		mov ah,0Eh		; Write to screen as TTY
		mov bx,0007h		; Attribute
		int 10h
		jmp short .loop
.return:	popad
		ret


;
; getlinsecsr: save registers, call getlinsec, restore registers
;	       Save/restore the flags, too, especially IF.
;
getlinsecsr:	
		pushfd
		pushad
		call getlinsec
		popad
		popfd
		ret

;
; Checksum error message
;
checksumerr_msg	db ' Load error - ', 0	; Boot failed appended

;
; BIOS type string
;
cbios_name	db 'CBIOS', 0
ebios_name	db 'EBIOS', 0

;
; Debug routine
;
%ifdef debug
safedumpregs:
		cmp word [Debug_Magic],0D00Dh
		jnz nc_return
		jmp dumpregs
%endif

rl_checkpt	equ $				; Must be <= 8000h

rl_checkpt_off	equ ($-$$)
%ifndef DEPEND
%if rl_checkpt_off > 400h
%error "Sector 1 overflow"
%endif
%endif

; ----------------------------------------------------------------------------
;  End of code and data that have to be in the first sector
; ----------------------------------------------------------------------------

all_read:
;
; Let the user (and programmer!) know we got this far.  This used to be
; in Sector 1, but makes a lot more sense here.
;
		mov si,copyright_str
		call writestr_early


;
; Insane hack to expand the DOS superblock to dwords
;
expand_super:
		xor eax,eax
		mov si,superblock
		mov di,SuperInfo
		mov cx,superinfo_size
.loop:
		lodsw
		dec si
		stosd				; Store expanded word
		xor ah,ah
		stosd				; Store expanded byte
		loop .loop

;
; Fall through to the mainline code...
;
		section .text
