[bits 32]
%define ERROR_CODE nop

%define ZERO push 0

extern idt_table ; idt_table 是 C 中注册的中断处理程序数组

section .data
global intr_entry_table
intr_entry_table:

%macro VECTOR 2
section .text
intr%1entry:
    %2 ;压入错误码
    push ds
    push es
    push fs
    push gs
    pushad ; 压入8个通用 32 位寄存器, 其入栈顺序是: eax, ecx, edx, ebx, esp, ebp, esi, edi

    ; 如果是从片上进入的中断，除了往从片上发送 EOI 外, 还要往主片上发送 EOI
    mov al, 0x20 ; 中断结束命令 EOI
    out 0xa0, al ; 向从片发送
    out 0x20, al ; 向主片发送

    push %1
    call [idt_table+%1*4] ; 调用 idt_table 中 C 版本中断处理函数
    jmp intr_exit

section .data
    dd intr%1entry ; 存储各个中断入口程序的地址
                   ; 形成 intr_entry_table 数组
%endmacro

section .text
global intr_exit
intr_exit:
; 恢复上下文环境
    add esp, 4 ; 跳过中断号
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 4
    iretd

VECTOR 0x00, ZERO
VECTOR 0x01, ZERO
VECTOR 0x02, ZERO
VECTOR 0x03, ZERO 
VECTOR 0x04, ZERO
VECTOR 0x05, ZERO
VECTOR 0x06, ZERO
VECTOR 0x07, ZERO 
VECTOR 0x08, ERROR_CODE
VECTOR 0x09, ZERO
VECTOR 0x0a, ERROR_CODE
VECTOR 0x0b, ERROR_CODE 
VECTOR 0x0c, ZERO
VECTOR 0x0d, ERROR_CODE
VECTOR 0x0e, ERROR_CODE
VECTOR 0x0f, ZERO 
VECTOR 0x10, ZERO
VECTOR 0x11, ERROR_CODE
VECTOR 0x12, ZERO
VECTOR 0x13, ZERO 
VECTOR 0x14, ZERO
VECTOR 0x15, ZERO
VECTOR 0x16, ZERO
VECTOR 0x17, ZERO 
VECTOR 0x18, ERROR_CODE
VECTOR 0x19, ZERO
VECTOR 0x1a, ERROR_CODE
VECTOR 0x1b, ERROR_CODE 
VECTOR 0x1c, ZERO
VECTOR 0x1d, ERROR_CODE
VECTOR 0x1e, ERROR_CODE
VECTOR 0x1f, ZERO 
VECTOR 0x20, ZERO   ;时钟中断
VECTOR 0x21,ZERO	;键盘中断对应的入口
VECTOR 0x22,ZERO	;级联用的
VECTOR 0x23,ZERO	;串口2对应的入口
VECTOR 0x24,ZERO	;串口1对应的入口
VECTOR 0x25,ZERO	;并口2对应的入口
VECTOR 0x26,ZERO	;软盘对应的入口
VECTOR 0x27,ZERO	;并口1对应的入口
VECTOR 0x28,ZERO	;实时时钟对应的入口
VECTOR 0x29,ZERO	;重定向
VECTOR 0x2a,ZERO	;保留
VECTOR 0x2b,ZERO	;保留
VECTOR 0x2c,ZERO	;ps/2鼠标
VECTOR 0x2d,ZERO	;fpu浮点单元异常
VECTOR 0x2e,ZERO	;硬盘
VECTOR 0x2f,ZERO	;保留


; 0x80 号中断
[bits 32]
extern syscall_table
section .text
global syscall_handler
syscall_handler:
; 1. 保存上下文环境
    push 0  ; 压入 0, 使栈中格式统一

    push ds
    push es
    push fs
    push gs
    pushad  ; PUSHAD 指令压入 32 位寄存器，其入栈顺序是:
            ; EAX, ECS, EDX, EBX, ESP, EBP, ESI, EDI

    push 0x80 ; 此位置压入 0x80 也是为了保持统一的栈格式
; 2. 为系统调用子功能传入参数
    push edx    ; 系统调用中第 3 个参数
    push ecx    ; 系统调用中第 2 个参数
    push ebx    ; 系统调用中第 1 个参数

; 3. 调用子功能处理函数
    call [syscall_table + eax * 4]
    add esp, 12 ; 跳过上面的 3 个参数

; 4. 将 call 调用后的返回值存入待当前内核栈中 eax 的位置, 这样执行中断退出例程popad时，eax寄存器会得到正确的返回值
    mov [esp + 8 * 4], eax
    jmp intr_exit   ; intr_exit 返回, 恢复上下文