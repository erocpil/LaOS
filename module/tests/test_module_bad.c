/* 负向载入测试夹具：内核导出表中不存在此符号。载入本模块应失败，且后续任务仍须运行。 */
extern void laos_missing_symbol_for_negative_test(void);

int laos_module_entry(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	laos_missing_symbol_for_negative_test();
	return 0;
}
