/* Negative loader fixture: this symbol must never exist in the kernel export
 * table. Loading the module must fail without stopping later tasks. */
extern void laos_missing_symbol_for_negative_test(void);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	laos_missing_symbol_for_negative_test();
	return 0;
}
