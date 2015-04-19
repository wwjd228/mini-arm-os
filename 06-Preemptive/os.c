#include <stddef.h>
#include <stdint.h>
#include "reg.h"
#include "asm.h"
#include "host.h"

/* Size of our user task stacks in words */
#define STACK_SIZE	256

/* Number of user task */
#define TASK_LIMIT	3

/* USART TXE Flag
 * This flag is cleared when data is written to USARTx_DR and
 * set when that data is transferred to the TDR
 */
#define USART_FLAG_TXE	((uint16_t) 0x0080)

int snprintf(char *buf, size_t size, const char *format, ...);
unsigned int get_time();
unsigned long tick_count = 0;


void usart_init(void)
{
	*(RCC_APB2ENR) |= (uint32_t) (0x00000001 | 0x00000004);
	*(RCC_APB1ENR) |= (uint32_t) (0x00020000);

	/* USART2 Configuration, Rx->PA3, Tx->PA2 */
	*(GPIOA_CRL) = 0x00004B00;
	*(GPIOA_CRH) = 0x44444444;
	*(GPIOA_ODR) = 0x00000000;
	*(GPIOA_BSRR) = 0x00000000;
	*(GPIOA_BRR) = 0x00000000;

	*(USART2_CR1) = 0x0000000C;
	*(USART2_CR2) = 0x00000000;
	*(USART2_CR3) = 0x00000000;
	*(USART2_CR1) |= 0x2000;
}

void print_str(const char *str)
{
	while (*str) {
		while (!(*(USART2_SR) & USART_FLAG_TXE));
		*(USART2_DR) = (*str & 0xFF);
		str++;
	}
}

void delay(int count)
{
	count *= 50000;
	while (count--);
}

/* Exception return behavior */
#define HANDLER_MSP	0xFFFFFFF1
#define THREAD_MSP	0xFFFFFFF9
#define THREAD_PSP	0xFFFFFFFD

/* Initilize user task stack and execute it one time */
/* XXX: Implementation of task creation is a little bit tricky. In fact,
 * after the second time we called `activate()` which is returning from
 * exception. But the first time we called `activate()` which is not returning
 * from exception. Thus, we have to set different `lr` value.
 * First time, we should set function address to `lr` directly. And after the
 * second time, we should set `THREAD_PSP` to `lr` so that exception return
 * works correctly.
 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Babefdjc.html
 */
unsigned int *create_task(unsigned int *stack, void (*start)(void))
{
	static int first = 1;

	stack += STACK_SIZE - 32; /* End of stack, minus what we are about to push */
	if (first) {
		stack[8] = (unsigned int) start;
		first = 0;
	} else {
		stack[8] = (unsigned int) THREAD_PSP;
		stack[15] = (unsigned int) start;
		stack[16] = (unsigned int) 0x01000000; /* PSR Thumb bit */
	}
	stack = activate(stack);

	return stack;
}

void task1_func(void)
{
	print_str("task1: Created!\n");
	print_str("task1: Now, return to kernel mode\n");
	syscall();
	while (1) {
		print_str("task1: Running...\n");
		delay(1000);
	}
}

void task2_func(void)
{
	print_str("task2: Created!\n");
	print_str("task2: Now, return to kernel mode\n");
	syscall();
	while (1) {
		print_str("task2: Running...\n");
		delay(1000);
	}
}

int main(void)
{
	unsigned int user_stacks[TASK_LIMIT][STACK_SIZE];
	unsigned int *usertasks[TASK_LIMIT];
	unsigned int timestamp;
	size_t task_count = 0;
	size_t current_task;

	usart_init();
	int handle = host_action(SYS_OPEN, "syslog", 4);

	print_str("OS: Starting...\n");
	print_str("OS: First create task 1\n");
	usertasks[0] = create_task(user_stacks[0], &task1_func);
	task_count += 1;
	print_str("OS: Back to OS, create task 2\n");
	usertasks[1] = create_task(user_stacks[1], &task2_func);
	task_count += 1;

	print_str("\nOS: Start round-robin scheduler!\n");

	/* SysTick configuration */
	*SYSTICK_LOAD = 7200000;
	*SYSTICK_VAL = 0;
	*SYSTICK_CTRL = 0x07;
	current_task = 0;

	while (1) {
		char buf_in[128];
		char buf_out[128];
		print_str("OS: Activate next task\n");
		timestamp = get_time();
		int len = snprintf(buf_in, 128, "task switch%d : in , timestamp : %d\n", current_task, timestamp);
		host_action(SYS_WRITE, handle, buf_in, len);
		usertasks[current_task] = activate(usertasks[current_task]);
		tick_count++;
		timestamp = get_time();
		len = snprintf(buf_out, 128, "task switch%d : out, timestamp : %d\n", current_task, timestamp);
		host_action(SYS_WRITE, handle, buf_out, len);
		print_str("OS: Back to OS\n");
		current_task = current_task == (task_count - 1) ? 0 : current_task + 1;
	}
	
	return 0;
}

unsigned int get_time()
{
	static unsigned int const *reload = (void *) 0xE000E014;
	static unsigned int const *current = (void *) 0xE000E018;
	return tick_count * (*reload) + (*reload - *current);
}

int _snprintf_int(int num, char *buf, int buf_size)
{
	int len = 1;
	char *p;
	int i = num < 0 ? -num : num;

	for (; i >= 10; i /= 10, len++);

	if (num < 0)
		len++;

	i = num;
	p = buf + len - 1;
	do {
		if (p < buf + buf_size)
			*p-- = '0' + i % 10;
		i /= 10;
	} while (i != 0);

	if (num < 0)
		*p = '-';

	return len < buf_size ? len : buf_size;
}

int snprintf(char *buf, size_t size, const char *format, ...)
{
	va_list ap;
        char *dest = buf;
	char *last = buf + size; 
        char ch;      

	va_start(ap, format);
	for (ch = *format++; dest < last && ch; ch = *format++) {
		if (ch == '%') {
			ch = *format++;
			switch (ch) {
			case 's' : {
					char *str = va_arg(ap, char*);
					/* strncpy */
					while (dest < last) {
						if ((*dest = *str++))
							dest++;
						else
							break;
					}
				}
				break;
			case 'd' : {
					int num = va_arg(ap, int);
					dest += _snprintf_int(num, dest, last - dest);
				}
				break;
			case '%' : 
				*dest++ = ch;
				break;
			default :
				return -1;

			}
		} else {
			*dest++ = ch;
		}
	}
	va_end(ap);

	if (dest < last)
		*dest = 0;

	else
		*--dest = 0;

	return dest - buf;
}
