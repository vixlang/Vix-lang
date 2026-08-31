default rel
section .text
extern printf
extern calloc
extern strcmp
extern strlen
extern vix_array_len
extern vix_array_push_i32
extern vix_array_push_ptr
extern vix_array_push_bytes
extern vix_array_slice_bytes
extern vix_safe_sdiv_i32
extern vix_safe_srem_i32
extern vix_string_concat
extern vix_string_slice
global main
main:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    mov eax, 0
    mov dword [rbp-16], eax
    mov dword [rbp-8], eax
    mov edi, 3
    mov al, 0
    call f
    mov dword [rbp-24], eax
    sub eax, 5
    mov dword [rbp-32], eax
    jmp main__return
main__return:
    mov rsp, rbp
    pop rbp
    ret
global __lambda_main_0
__lambda_main_0:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    mov dword [rbp-8], edi
    mov eax, dword [rbp-8]
    mov dword [rbp-16], eax
    mov dword [rbp-24], eax
    add eax, 2
    mov dword [rbp-32], eax
    jmp __lambda_main_0__return
__lambda_main_0__return:
    mov rsp, rbp
    pop rbp
    ret
