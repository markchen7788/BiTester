/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include "dpdk_util.c"
#include "flow_block.c"
#include "parser.c"
#include "process.c"

// 100Gbps
int main(int argc, char **argv)
{
	uint8_t nb_queues = 1;
	int nb_ports = 0;
	char configFileName[100] = {0};
	char *helpInfo = "Usage: BiTester -a PCIE -a PCIE -- -r Port -t Port -n queueNum -s speed -f configFile\n";

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-n") == 0) // normal paired queue
		{
			nb_queues = atoi(argv[++i]);
		}
		if (strcmp(argv[i], "-s") == 0) // speed
		{
			speed = atof(argv[++i]);
			converge = false;
		}
		if (strcmp(argv[i], "-r") == 0) // rx-port
		{
			RX_PORT = atoi(argv[++i]);
		}
		if (strcmp(argv[i], "-t") == 0) // tx-port
		{
			TX_PORT = atoi(argv[++i]);
		}
		if (strcmp(argv[i], "-f") == 0) // tx-port
		{
			strcpy(configFileName, argv[++i]);
		}
		if (strcmp(argv[i], "-h") == 0) // tx-port
		{
			printf("%s", helpInfo);
			return 0;
		}
	}
	if (RX_PORT == -1 || TX_PORT == -1)
	{
		RX_PORT = 0;
		TX_PORT = 0;
	}
	if (RX_PORT == TX_PORT)
	{
		nb_ports = 1;
	}
	else
	{
		nb_ports = 2;
	}
	if (configFileName[0] == 0)
	{
		printf("please choose a config file......\n");
		printf("%s", helpInfo);
		return 0;
	}

	dpdk_init(argc, argv, nb_ports, nb_queues); // 初始化dpdk参数
	parser(&flows, configFileName);

	// if (flows.mode) // 如果进行时延测试
	// {
	// 	addRteFlow(); // 下发流表
	// }

	start();
	dpdk_destroy(nb_ports); // 回收资源
	return 0;
}
