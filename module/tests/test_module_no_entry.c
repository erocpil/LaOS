/* 旧 ABI 负向测试夹具：模块载入器不得接受 main() 作为模块入口。 */
int module_no_entry_data = 42;

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}
