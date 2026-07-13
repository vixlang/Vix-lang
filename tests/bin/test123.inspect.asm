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
extern vix_string_concat
global ImportedNum
ImportedNum:
    push rbp
    mov rbp, rsp
    sub rsp, 224
    mov qword [rbp-8], rdi
    mov dword [rbp-16], esi
    mov eax, dword [rbp-16]
    mov dword [rbp-24], eax
    mov qword [rbp-56], 0
    mov qword [rbp-48], 0
    mov qword [rbp-40], 0
    mov qword [rbp-32], 0
    mov rax, qword [rbp-56]
    mov qword [rbp-88], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-80], rax
    mov rax, qword [rbp-40]
    mov qword [rbp-72], rax
    mov rax, qword [rbp-32]
    mov qword [rbp-64], rax
    mov eax, 0
    mov dword [rbp-88], eax
    mov rax, qword [rbp-88]
    mov qword [rbp-120], rax
    mov rax, qword [rbp-80]
    mov qword [rbp-112], rax
    mov rax, qword [rbp-72]
    mov qword [rbp-104], rax
    mov rax, qword [rbp-64]
    mov qword [rbp-96], rax
    mov eax, dword [rbp-24]
    mov dword [rbp-116], eax
    lea rax, [Lstr0]
    mov qword [rbp-128], rax
    mov rax, qword [rbp-120]
    mov qword [rbp-160], rax
    mov rax, qword [rbp-112]
    mov qword [rbp-152], rax
    mov rax, qword [rbp-104]
    mov qword [rbp-144], rax
    mov rax, qword [rbp-96]
    mov qword [rbp-136], rax
    mov rax, qword [rbp-128]
    mov qword [rbp-152], rax
    mov rax, qword [rbp-160]
    mov qword [rbp-192], rax
    mov rax, qword [rbp-152]
    mov qword [rbp-184], rax
    mov rax, qword [rbp-144]
    mov qword [rbp-176], rax
    mov rax, qword [rbp-136]
    mov qword [rbp-168], rax
    mov rax, 0
    mov qword [rbp-176], rax
    mov rax, qword [rbp-192]
    mov qword [rbp-224], rax
    mov rax, qword [rbp-184]
    mov qword [rbp-216], rax
    mov rax, qword [rbp-176]
    mov qword [rbp-208], rax
    mov rax, qword [rbp-168]
    mov qword [rbp-200], rax
    mov rax, 6.95308e-310
    mov qword [rbp-200], rax
    mov r11, qword [rbp-8]
    mov rax, qword [rbp-224]
    mov qword [r11+0], rax
    mov rax, qword [rbp-216]
    mov qword [r11+8], rax
    mov rax, qword [rbp-208]
    mov qword [r11+16], rax
    mov rax, qword [rbp-200]
    mov qword [r11+24], rax
    mov rax, qword [rbp-8]
    jmp ImportedNum__return
ImportedNum__return:
    mov rsp, rbp
    pop rbp
    ret
global ImportedNil
ImportedNil:
    push rbp
    mov rbp, rsp
    sub rsp, 208
    mov qword [rbp-8], rdi
    mov qword [rbp-40], 0
    mov qword [rbp-32], 0
    mov qword [rbp-24], 0
    mov qword [rbp-16], 0
    mov rax, qword [rbp-40]
    mov qword [rbp-72], rax
    mov rax, qword [rbp-32]
    mov qword [rbp-64], rax
    mov rax, qword [rbp-24]
    mov qword [rbp-56], rax
    mov rax, qword [rbp-16]
    mov qword [rbp-48], rax
    mov eax, 1
    mov dword [rbp-72], eax
    mov rax, qword [rbp-72]
    mov qword [rbp-104], rax
    mov rax, qword [rbp-64]
    mov qword [rbp-96], rax
    mov rax, qword [rbp-56]
    mov qword [rbp-88], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-80], rax
    mov eax, 0
    mov dword [rbp-100], eax
    lea rax, [Lstr0]
    mov qword [rbp-112], rax
    mov rax, qword [rbp-104]
    mov qword [rbp-144], rax
    mov rax, qword [rbp-96]
    mov qword [rbp-136], rax
    mov rax, qword [rbp-88]
    mov qword [rbp-128], rax
    mov rax, qword [rbp-80]
    mov qword [rbp-120], rax
    mov rax, qword [rbp-112]
    mov qword [rbp-136], rax
    mov rax, qword [rbp-144]
    mov qword [rbp-176], rax
    mov rax, qword [rbp-136]
    mov qword [rbp-168], rax
    mov rax, qword [rbp-128]
    mov qword [rbp-160], rax
    mov rax, qword [rbp-120]
    mov qword [rbp-152], rax
    mov rax, 0
    mov qword [rbp-160], rax
    mov rax, qword [rbp-176]
    mov qword [rbp-208], rax
    mov rax, qword [rbp-168]
    mov qword [rbp-200], rax
    mov rax, qword [rbp-160]
    mov qword [rbp-192], rax
    mov rax, qword [rbp-152]
    mov qword [rbp-184], rax
    mov rax, 6.95308e-310
    mov qword [rbp-184], rax
    mov r11, qword [rbp-8]
    mov rax, qword [rbp-208]
    mov qword [r11+0], rax
    mov rax, qword [rbp-200]
    mov qword [r11+8], rax
    mov rax, qword [rbp-192]
    mov qword [r11+16], rax
    mov rax, qword [rbp-184]
    mov qword [r11+24], rax
    mov rax, qword [rbp-8]
    jmp ImportedNil__return
ImportedNil__return:
    mov rsp, rbp
    pop rbp
    ret
global eval
eval:
    push rbp
    mov rbp, rsp
    sub rsp, 208
    mov r11, rdi
    mov rax, qword [r11+0]
    mov qword [rbp-32], rax
    mov rax, qword [r11+8]
    mov qword [rbp-24], rax
    mov rax, qword [r11+16]
    mov qword [rbp-16], rax
    mov rax, qword [r11+24]
    mov qword [rbp-8], rax
    mov rax, qword [rbp-32]
    mov qword [rbp-64], rax
    mov rax, qword [rbp-24]
    mov qword [rbp-56], rax
    mov rax, qword [rbp-16]
    mov qword [rbp-48], rax
    mov rax, qword [rbp-8]
    mov qword [rbp-40], rax
    mov rax, qword [rbp-64]
    mov qword [rbp-96], rax
    mov rax, qword [rbp-56]
    mov qword [rbp-88], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-80], rax
    mov rax, qword [rbp-40]
    mov qword [rbp-72], rax
    mov eax, dword [rbp-96]
    mov dword [rbp-104], eax
    mov eax, dword [rbp-104]
    cmp eax, 0
    sete al
    movzx eax, al
    mov dword [rbp-112], eax
    cmp dword [rbp-112], 0
    je eval__if_else1
    jmp eval__if_then0
eval__if_then0:
    mov rax, qword [rbp-64]
    mov qword [rbp-152], rax
    mov rax, qword [rbp-56]
    mov qword [rbp-144], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-136], rax
    mov rax, qword [rbp-40]
    mov qword [rbp-128], rax
    mov eax, dword [rbp-148]
    mov dword [rbp-160], eax
    mov eax, dword [rbp-160]
    mov dword [rbp-120], eax
    mov eax, dword [rbp-120]
    jmp eval__return
eval__if_else1:
    mov rax, qword [rbp-64]
    mov qword [rbp-192], rax
    mov rax, qword [rbp-56]
    mov qword [rbp-184], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-176], rax
    mov rax, qword [rbp-40]
    mov qword [rbp-168], rax
    mov eax, dword [rbp-192]
    mov dword [rbp-200], eax
    mov eax, dword [rbp-200]
    cmp eax, 1
    sete al
    movzx eax, al
    mov dword [rbp-208], eax
    cmp dword [rbp-208], 0
    je eval__if_else4
    jmp eval__if_then3
eval__if_then3:
    mov eax, 0
    jmp eval__return
eval__if_else4:
    jmp eval__if_end5
eval__if_end5:
    jmp eval__if_end2
eval__if_end2:
    mov eax, 0
    jmp eval__return
eval__return:
    mov rsp, rbp
    pop rbp
    ret
global main
main:
    push rbp
    mov rbp, rsp
    sub rsp, 560
    mov qword [rbp-64], 0
    mov qword [rbp-56], 0
    mov qword [rbp-48], 0
    mov qword [rbp-40], 0
    mov rax, qword [rbp-64]
    mov qword [rbp-96], rax
    mov rax, qword [rbp-56]
    mov qword [rbp-88], rax
    mov rax, qword [rbp-48]
    mov qword [rbp-80], rax
    mov rax, qword [rbp-40]
    mov qword [rbp-72], rax
    mov eax, 0
    mov dword [rbp-96], eax
    mov rax, qword [rbp-96]
    mov qword [rbp-128], rax
    mov rax, qword [rbp-88]
    mov qword [rbp-120], rax
    mov rax, qword [rbp-80]
    mov qword [rbp-112], rax
    mov rax, qword [rbp-72]
    mov qword [rbp-104], rax
    mov eax, 42
    mov dword [rbp-124], eax
    lea rax, [Lstr0]
    mov qword [rbp-136], rax
    mov rax, qword [rbp-128]
    mov qword [rbp-168], rax
    mov rax, qword [rbp-120]
    mov qword [rbp-160], rax
    mov rax, qword [rbp-112]
    mov qword [rbp-152], rax
    mov rax, qword [rbp-104]
    mov qword [rbp-144], rax
    mov rax, qword [rbp-136]
    mov qword [rbp-160], rax
    mov rax, qword [rbp-168]
    mov qword [rbp-200], rax
    mov rax, qword [rbp-160]
    mov qword [rbp-192], rax
    mov rax, qword [rbp-152]
    mov qword [rbp-184], rax
    mov rax, qword [rbp-144]
    mov qword [rbp-176], rax
    mov rax, 0
    mov qword [rbp-184], rax
    mov rax, qword [rbp-200]
    mov qword [rbp-232], rax
    mov rax, qword [rbp-192]
    mov qword [rbp-224], rax
    mov rax, qword [rbp-184]
    mov qword [rbp-216], rax
    mov rax, qword [rbp-176]
    mov qword [rbp-208], rax
    mov rax, 6.95308e-310
    mov qword [rbp-208], rax
    mov rax, qword [rbp-232]
    mov qword [rbp-32], rax
    mov rax, qword [rbp-224]
    mov qword [rbp-24], rax
    mov rax, qword [rbp-216]
    mov qword [rbp-16], rax
    mov rax, qword [rbp-208]
    mov qword [rbp-8], rax
    mov qword [rbp-296], 0
    mov qword [rbp-288], 0
    mov qword [rbp-280], 0
    mov qword [rbp-272], 0
    mov rax, qword [rbp-296]
    mov qword [rbp-328], rax
    mov rax, qword [rbp-288]
    mov qword [rbp-320], rax
    mov rax, qword [rbp-280]
    mov qword [rbp-312], rax
    mov rax, qword [rbp-272]
    mov qword [rbp-304], rax
    mov eax, 1
    mov dword [rbp-328], eax
    mov rax, qword [rbp-328]
    mov qword [rbp-360], rax
    mov rax, qword [rbp-320]
    mov qword [rbp-352], rax
    mov rax, qword [rbp-312]
    mov qword [rbp-344], rax
    mov rax, qword [rbp-304]
    mov qword [rbp-336], rax
    mov eax, 0
    mov dword [rbp-356], eax
    lea rax, [Lstr0]
    mov qword [rbp-368], rax
    mov rax, qword [rbp-360]
    mov qword [rbp-400], rax
    mov rax, qword [rbp-352]
    mov qword [rbp-392], rax
    mov rax, qword [rbp-344]
    mov qword [rbp-384], rax
    mov rax, qword [rbp-336]
    mov qword [rbp-376], rax
    mov rax, qword [rbp-368]
    mov qword [rbp-392], rax
    mov rax, qword [rbp-400]
    mov qword [rbp-432], rax
    mov rax, qword [rbp-392]
    mov qword [rbp-424], rax
    mov rax, qword [rbp-384]
    mov qword [rbp-416], rax
    mov rax, qword [rbp-376]
    mov qword [rbp-408], rax
    mov rax, 0
    mov qword [rbp-416], rax
    mov rax, qword [rbp-432]
    mov qword [rbp-464], rax
    mov rax, qword [rbp-424]
    mov qword [rbp-456], rax
    mov rax, qword [rbp-416]
    mov qword [rbp-448], rax
    mov rax, qword [rbp-408]
    mov qword [rbp-440], rax
    mov rax, 6.95308e-310
    mov qword [rbp-440], rax
    mov rax, qword [rbp-464]
    mov qword [rbp-264], rax
    mov rax, qword [rbp-456]
    mov qword [rbp-256], rax
    mov rax, qword [rbp-448]
    mov qword [rbp-248], rax
    mov rax, qword [rbp-440]
    mov qword [rbp-240], rax
    mov rax, qword [rbp-32]
    mov qword [rbp-496], rax
    mov rax, qword [rbp-24]
    mov qword [rbp-488], rax
    mov rax, qword [rbp-16]
    mov qword [rbp-480], rax
    mov rax, qword [rbp-8]
    mov qword [rbp-472], rax
    lea rdi, [rbp-496]
    mov al, 0
    call eval
    mov dword [rbp-504], eax
    mov rax, qword [rbp-264]
    mov qword [rbp-536], rax
    mov rax, qword [rbp-256]
    mov qword [rbp-528], rax
    mov rax, qword [rbp-248]
    mov qword [rbp-520], rax
    mov rax, qword [rbp-240]
    mov qword [rbp-512], rax
    lea rdi, [rbp-536]
    mov al, 0
    call eval
    mov dword [rbp-544], eax
    mov eax, dword [rbp-504]
    add eax, dword [rbp-544]
    mov dword [rbp-552], eax
    mov eax, dword [rbp-552]
    jmp main__return
main__return:
    mov rsp, rbp
    pop rbp
    ret
section .data
Lstr0: db 0
