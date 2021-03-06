//zeromq_pull_fixed_position.c
#include "shiki.h"

#define VELOCITY_MODE_MAX_SPEED 1500000
#define VELOCITY_MODE_MAX_ACC 2000000
#define SELF_CHECK_POT_VALUE 3.2
#define SELF_CHECK_FORCE_VALUE 200
#define MOTOR_ENCODER_DIRECTION 1


#define PULL_FIX_POSITION 0
#define PULL_FORCE_TORQUE 1

#define GAIT_B_MODE PULL_FORCE_TORQUE

int motor_ctl(char *msg, void *para,struct motor_ctl_t *rev,int port);

void thread_force_port(void);
void thread_pot_port(void);
void thread_motor_port(void);
void thread_gait_zeromq(void);
int thread_client_zeromq(void);
void thread_motor_module_run_info(void);
void thread_test_press(void);

struct timespec gettimeout(uint32_t timeout);

pthread_mutex_t mutex_force;
pthread_mutex_t mutex_pot;
pthread_mutex_t mutex_gait_msg;
pthread_mutex_t mutex_client_msg;
pthread_mutex_t mutex_info;
pthread_mutex_t mutex_foot_test;


sem_t sem_client;
sem_t sem_motor;
sem_t sem_pot_check;
sem_t sem_force_check;
sem_t sem_self_check;

uint32_t force_now;										//串口上报的力传感器值
float pot_now;
uint32_t state_now;										//socket收到的电机运动状态命令
int32_t motor_en_flag;										//1:工作   0:不工作   通过互斥锁操作
struct motor_module_run_info_t motor_module_run_info;
struct motor_module_check_info_t motor_module_check_info;
struct motor_para_init_t motor_para_init;
char foot_test[256];

//获取力传感器数据，一帧的格式为 帧头：0x53 数据：0x** 0x** 校验：两个数据的与 帧尾：0x59
int get_force(int port,uint32_t *msg)
{

    int32_t 		readcnt = 0,nread,ncheck=0,state = 0;
    uint8_t			data[256],datagram[64];
    float	  		adc_temp;
    uint32_t 		force_temp;
    int try_t = 256;
    while(try_t--){
        datagram[0] = 0;
        nread = (int32_t)read(port,datagram,1);
        if(nread<0){
            printf("read error nread=%d\n",readcnt);
            return -1;
        }

        if(nread == 0){
            return -1;
        }
        switch(state){
            case 0:
                if(datagram[0]==0x53){
                    data[readcnt] = datagram[0];
                    readcnt++;
                    state=1;
                }
                break;
            case 1:
                if(readcnt < 3){
                    data[readcnt] = datagram[0];
                    readcnt++;
                    break;
                }else{
                    data[readcnt] = datagram[0];
                    if(data[3] == data[1]^data[2]){
                        state=2;
                        readcnt++;
                        break;
                    }else{

                        state = 0;
                        readcnt = 0;

                        pthread_mutex_lock(&mutex_info);
                        motor_module_run_info.force_senser_error++;		//校验错误，收集错误信息
                        pthread_mutex_unlock(&mutex_info);

                        break;
                    }
                }
            case 2:
                if(datagram[0]==0x59){
                    data[readcnt] = datagram[0];
                    force_temp = (uint32_t)((data[2]<<8)|data[1]);
                    state = 0;
                    readcnt= 0;
                    *msg = force_temp;
                    return 0;
                    break;
                }else{

                    state = 0;
                    readcnt= 0;

                    pthread_mutex_lock(&mutex_info);
                    motor_module_run_info.force_senser_error++;		//校验错误，收集错误信息
                    pthread_mutex_unlock(&mutex_info);

                    break;
                }
            default:
                break;
        }
    }
    return -1;
}

int get_pot(int port,float *msg)
{

    int32_t 		readcnt = 0,nread,ncheck=0,state = 0;
    uint8_t			data[256],datagram[64];
    float	  		adc_temp;
    int try_t = 256;
    while(try_t--){
        datagram[0] = 0;
        nread = (int32_t)read(port,datagram,1);

        if(nread<0){
            return -1;
        }

        if(nread == 0){
            return -1;
        }

        switch(state){
            case 0:
                if(datagram[0]==0x53){
                    data[readcnt] = datagram[0];
                    readcnt++;
                    state=1;
                }
                break;
            case 1:
                if(readcnt < 3){
                    data[readcnt] = datagram[0];
                    readcnt++;
                    break;
                }else{
                    data[readcnt] = datagram[0];
                    if(data[3] == data[1]^data[2]){
                        state=2;
                        readcnt++;
                        break;
                    }else{
                        state = 0;
                        readcnt = 0;

                        pthread_mutex_lock(&mutex_info);
                        motor_module_run_info.pot_senser_error++;		//校验错误，收集错误信息
                        pthread_mutex_unlock(&mutex_info);

                        break;
                    }
                }
            case 2:
                if(datagram[0]==0x59){
                    data[readcnt] = datagram[0];
                    adc_temp = ((float)((data[2]<<8)|data[1]))/1000;
                    state = 0;
                    readcnt= 0;
                    *msg = adc_temp;
                    return 0;
                    break;
                }
                else{
                    state = 0;
                    readcnt= 0;

                    pthread_mutex_lock(&mutex_info);
                    motor_module_run_info.pot_senser_error++;		//校验错误，收集错误信息
                    pthread_mutex_unlock(&mutex_info);

                    break;
                }
            default:
                break;
        }
    }
    return -1;
}


//驱动电机的接口，通过串口ASCII码的格式与驱动器通讯，通讯分两种
//一种是带有参数的，需要将参数传入到para
//不带参数的para传入NULL即可
//shiki.h文件里有所需要的通讯格式的宏定义
//返回为驱动器的反馈（v ***; ok; e **;）
int motor_ctl(char *msg, void *para,struct motor_ctl_t *rev,int port)
{
    int32_t readcnt = 0,nread;
    char com[20];
    uint8_t data[256],datagram[64];
    int *p = para;
    int try_cmd = 5;
    int try_t = 1024;
    struct motor_ctl_t temp;

    if(para == NULL){
        sprintf(com,msg,NULL);
    }else{
        sprintf(com,msg,*p);
    }
    while(try_cmd--){
        write(port,com,strlen(com));		//发送串口命令

        memset(data,'\0',256);
        while(try_t--){
            nread = (int32_t)read(port,datagram,1);	//获取串口命令
            if(nread<0){
                return -1;
            }
            if(nread == 0){
                printf("Communication fail\n");

                pthread_mutex_lock(&mutex_info);
                motor_module_run_info.motor_driver_error++;		//通讯失败错误信息累计
                pthread_mutex_unlock(&mutex_info);

                return -1;
            }
            data[readcnt] = datagram[0];
            readcnt++;
            if(datagram[0]==0x0D){	//最后一位为“\n”判断接收到最后一位后再处理数据
                memcpy(temp.com,data,sizeof(data));
                //			printf("rev = %s\n",temp.com);
                if(strstr(temp.com,"v")!=0){

                    sscanf(temp.com,"%*s%d",&temp.temp);
                    if((void*)rev != NULL){
                        memcpy(rev,&temp,sizeof(struct motor_ctl_t));
                    }
                    return 0;

                }else if(strstr(temp.com,"e")!=0){
                    sscanf(temp.com,"%*s%d",&temp.state);
                    printf("motor error = %d\n",temp.state);

                    pthread_mutex_lock(&mutex_info);
                    motor_module_run_info.motor_driver_error++;		//通讯失败错误信息累计
                    pthread_mutex_unlock(&mutex_info);
                    break;
                }else if(strstr(temp.com,"ok")!=0){
                    return 0;
                }
            }
        }
    }
    return -1;
}

//对力传感器的数据进行处理
//冒泡算法+平均，除去了最大和最小的一个数
uint32_t bubble_sort_and_average(void *msg, uint8_t len)
{
    uint32_t temp;
    uint32_t *s;
    int i,j,flag = 1;

    if(len < 3){
        return 0;
    }

    for(j=0;j<len-1&&flag;j++){
        s = msg;
        flag = 0;
        for(i=0;i<len-1-j;i++){
            if(*s > *(s +1)){
                temp = *s;
                *s = *(s+1);
                *(s+1) = temp;
                flag = 1;
            }
            s++;
        }
    }
    s = msg;
    s++;
    temp = 0;
    while((uint32_t*)s<(uint32_t*)msg+len-1){
        temp = temp + *s;
        s ++;
    }
    temp = (uint32_t)(temp/(len-2));
    return temp;
}


void main()
{
    pthread_t tid1,tid2,tid3,tid4,tid5,tid6,tid7;		//定义了四个线程

    pthread_mutex_init(&mutex_force,NULL);			//定义了三个互斥锁
    pthread_mutex_init(&mutex_pot,NULL);
    pthread_mutex_init(&mutex_gait_msg,NULL);
    pthread_mutex_init(&mutex_client_msg,NULL);
    pthread_mutex_init(&mutex_info,NULL);
    pthread_mutex_init(&mutex_foot_test,NULL);



    pthread_create(&tid1,NULL,(void*)thread_force_port,NULL);			//创建线程
    pthread_create(&tid2,NULL,(void*)thread_motor_port,NULL);
    pthread_create(&tid3,NULL,(void*)thread_gait_zeromq,NULL);
    pthread_create(&tid4,NULL,(void*)thread_client_zeromq,NULL);
    pthread_create(&tid5,NULL,(void*)thread_pot_port,NULL);
    pthread_create(&tid6,NULL,(void*)thread_motor_module_run_info,NULL);
    pthread_create(&tid7,NULL,(void*)thread_test_press,NULL);



    sem_init(&sem_client,0,0);
    sem_init(&sem_motor,0,0);


    if((pthread_join(tid1,NULL) == 0)\
		||(pthread_join(tid2,NULL) == 0)\
		||(pthread_join(tid3,NULL) == 0)\
		||(pthread_join(tid4,NULL) == 0)\
		||(pthread_join(tid5,NULL) == 0)\
		||(pthread_join(tid6,NULL) == 0)\

            ){

    }
}

//力传感器读取线程
void thread_force_port(void)
{
    int FrocePort;			//力传感器的端口号
    int ret;
    uint32_t force_t,force_temp[12];

    FrocePort = tty_init(FORCE_PORT_NUM);
    if(FrocePort<0){
        pthread_mutex_lock(&mutex_info);
        sprintf(motor_module_check_info.force_port_state,"%s",SENSER_NO_PORT);
        pthread_mutex_unlock(&mutex_info);
        sem_post(&sem_force_check);
    }

    driver_init(FrocePort,FORCE_PORT_NUM);

    ret = get_force(FrocePort,&force_temp[0]);

    if(ret == -1){

        pthread_mutex_lock(&mutex_info);
        sprintf(motor_module_check_info.force_port_state,"%s",SENSER_NO_DATA);
        pthread_mutex_unlock(&mutex_info);
        sem_post(&sem_force_check);

    }else if(ret == 0){

        pthread_mutex_lock(&mutex_info);
        sprintf(motor_module_check_info.force_port_state,"%s",SENSER_OK);
        pthread_mutex_unlock(&mutex_info);
        sem_post(&sem_force_check);

    }

    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    zmq_connect (requester, "tcp://192.168.1.8:8034");
    zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char* s;
    char buffer[32],force_str[32];
    int force_try = 1024;

    while(force_try){

        memset(buffer,0,sizeof(buffer));
        zmq_recv (requester, buffer, sizeof(buffer), 0);

        s = strstr(buffer,"force=");
        if(s != NULL){
            sprintf(force_str,"%s",s+strlen("force="));
            sscanf(force_str,"%d",&force_t);

            pthread_mutex_lock(&mutex_force);			//互斥锁
            force_now = force_t;
            pthread_mutex_unlock(&mutex_force);
            force_try = 1024;
        } else{
            force_try--;
        }

    }
    zmq_close (requester);
    zmq_ctx_destroy (context);
    close(FrocePort);
}


//电位计读取线程
void thread_pot_port(void)
{
    float pot_t;
    int32_t pot_temp;
    int pot_try = 1024;
    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    zmq_connect (requester, "tcp://localhost:8031");
    zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char* s;
    char buffer[32],port_str[32];

    memset(buffer,0,sizeof(buffer));
    zmq_recv (requester, buffer, sizeof(buffer), 0);

    pthread_mutex_lock(&mutex_info);
    sprintf(motor_module_check_info.pot_port_state,"%s",SENSER_NO_DATA);
    pthread_mutex_unlock(&mutex_info);
    sem_post(&sem_pot_check);

    while(pot_try){

        memset(buffer,0,sizeof(buffer));
        zmq_recv (requester, buffer, sizeof(buffer), 0);

        s = strstr(buffer,"pubofposition: ");
        if(s != NULL){
            sprintf(port_str,"%s",s+strlen("pubofposition: "));
            sscanf(port_str,"%d",&pot_temp);

            if(pot_temp<900){
                pot_temp = pot_temp + 3300;
            }
            pot_temp = pot_temp - 900;
            pot_t = (float)pot_temp/1000;
            pthread_mutex_lock(&mutex_pot);			//互斥锁
            pot_now = pot_t;
            pthread_mutex_unlock(&mutex_pot);
            pot_try = 1024;
        } else{
            pot_try--;
        }
    }

    zmq_close (requester);
    zmq_ctx_destroy (context);
}



//电机控制线程
void thread_motor_port(void)
{
    int MotorPort;
    int deltav_motor = 0,deltav_motor_old,motor_cmd_position,motor_cmd_velocity,max_position;
    float pot_temp;
    int i,nwrite,index,pndex,dndex,nset_acc,max_force_cnt,motor_speed_t,motor_speed_t_old;
    uint32_t state_temp,state_old = 0,gait_temp,force_temp,max_force;
    uint32_t time_now,time_mark,init_position[10];
    int32_t deltav_force = 0,integral_force = 0,init_force[10],deltav_force_old,derivative_force;
    struct timeval tv;
    struct timespec ts;
    int timeout = 1000,is_check = 0;
    struct motor_ctl_t motor_position,motor_state,motor_speed;
    int32_t motor_en_flag_temp,motor_en_flag_temp_old=0;
    struct motor_para_init_t motor_para_init_temp;

    MotorPort = tty_init(MOTOR_PORT_NUM);
    if(MotorPort<0){
        pthread_mutex_lock(&mutex_info);
        sprintf(motor_module_check_info.motor_state,"%s",MOTOR_NO_PORT);
        pthread_mutex_unlock(&mutex_info);
    }

    int driver_init_resualt = driver_init(MotorPort,MOTOR_PORT_NUM);
    if(driver_init_resualt == 0){
        pthread_mutex_lock(&mutex_info);
        sprintf(motor_module_check_info.motor_state,"%s",MOTOR_NO_DATA);
        pthread_mutex_unlock(&mutex_info);
    }

    time_t log_time = time(NULL);
    struct tm *log_tp = localtime(&log_time);
    char log_path[32];



    while(1){

        sem_wait(&sem_motor);
        pthread_mutex_lock(&mutex_client_msg);
        motor_para_init_temp = motor_para_init;
        pthread_mutex_unlock(&mutex_client_msg);
        max_position = motor_para_init_temp.max_position;
        deltav_motor_old = 0;
        max_force_cnt = 0;

        gettimeofday(&tv,NULL);
        time_mark = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);		//获取系统时间，单位为ms


        while(1){

            pthread_mutex_lock(&mutex_client_msg);			//获取当前使能状态
            motor_en_flag_temp = motor_en_flag;
            pthread_mutex_unlock(&mutex_client_msg);
//			motor_en_flag_temp = CTL_CMDMOTIONSTART;

            switch(motor_en_flag_temp){
                case CTL_CMDINITIAL:											//自检
                    if((motor_en_flag_temp_old == 0)||(is_check == 0)){

                        gettimeofday(&tv,NULL);
                        ts.tv_sec = tv.tv_sec;
                        ts.tv_nsec = tv.tv_usec*1000 + timeout*1000*1000;
                        ts.tv_sec += ts.tv_nsec/(1000*1000*1000);
                        ts.tv_nsec %= (1000*1000*1000);

/*                        if(motor_en_flag_temp_old == 0){
                            if((sem_timedwait(&sem_pot_check,&ts) == 0)&&(sem_timedwait(&sem_force_check,&ts) == 0)){
                                printf("success get senser data\n");
                                pthread_mutex_lock(&mutex_info);
                                motor_module_check_info.motor_module_check_results = on_checking;
                                pthread_mutex_unlock(&mutex_info);
                            }else{
                                printf("motor module self check abort:can not get senser data\n");
                                pthread_mutex_lock(&mutex_info);
                                motor_module_check_info.motor_module_check_results = no_sensor_data;
                                pthread_mutex_unlock(&mutex_info);
                                is_check = 1;
                                sem_post(&sem_client);
                                break;
                            }
                        }*/

                        nwrite = VELOCITY_MODE_MAX_ACC;
                        motor_ctl(SET_VELOCITY_ACC,&nwrite,NULL,MotorPort);

                        nwrite = VELOCITY_MODE_MAX_ACC;
                        motor_ctl(SET_VELOCITY_DEC,&nwrite,NULL,MotorPort);

                        nset_acc = motor_para_init_temp.nset_acc;											//驱动器的最大加速度设置参数
                        motor_ctl(SET_MAX_DEC,&nset_acc,NULL,MotorPort);
                        nset_acc = motor_para_init_temp.nset_acc;
                        motor_ctl(SET_MAX_ACC,&nset_acc,NULL,MotorPort);

                        motor_cmd_velocity = 200000;																		//设置运动速度为14000rpm 此参数需要可以配置
                        motor_ctl(SET_VELOCITY,&motor_cmd_velocity,NULL,MotorPort);

                        //寻零自检，循环次数超过100次认为无法达到
/*                        int pot_value_try = 200;

                        while(pot_value_try--){

                            nwrite = ENABLE_POSITION_MODE;
                            motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);

                            motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);

                            pthread_mutex_lock(&mutex_pot);			//互斥锁
                            pot_temp = pot_now;
                            pthread_mutex_unlock(&mutex_pot);

                            if((pot_temp>(SELF_CHECK_POT_VALUE+0.02))||(pot_temp<(SELF_CHECK_POT_VALUE-0.08))){

                                motor_cmd_position = motor_position.temp + (int)(MOTOR_ENCODER_DIRECTION*(SELF_CHECK_POT_VALUE - pot_temp)*10000);
                                motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                                motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);
                                printf("what is pot_temp: %f pot_value_try:%d\n",pot_temp,pot_value_try);

                            }else{
                                motor_ctl(TRAJECTORY_ABORT,NULL,NULL,MotorPort);
                                nwrite = 100000;
                                motor_ctl(SET_POSITION,&nwrite,NULL,MotorPort);
                                printf("what is pot_temp: %f try_cnt:%d\n",pot_temp,200 - pot_value_try);
                                break;
                            }

                        }

                        if(pot_value_try <= 0){
                            printf("motor module self check abort:can not reach zero position\n");
                            pthread_mutex_lock(&mutex_info);
                            motor_module_check_info.motor_module_check_results = unreachable_zero_position;
                            pthread_mutex_unlock(&mutex_info);
                            is_check = 1;
                            sem_post(&sem_client);
                            break;
                        }*/

                        motor_cmd_velocity = 100000;
                        motor_ctl(SET_VELOCITY,&motor_cmd_velocity,NULL,MotorPort);

                        //预紧力点自适应，电位计位置超出或者循环超过1分钟，认为自检失败

/*					uint32_t init_mark;

					gettimeofday(&tv,NULL);
					init_mark = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
					init_mark = (init_mark - time_mark)&0x000fffff;

					while(1){

						pthread_mutex_lock(&mutex_pot);			//互斥锁
						pot_temp = pot_now;
						pthread_mutex_unlock(&mutex_pot);
						gettimeofday(&tv,NULL);

						time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
						time_now = (time_now - time_mark)&0x000fffff;

						if((pot_temp>0.1)&&(pot_temp<3.3)&&((time_now - init_mark) < 60000)){

						}else{
							printf("motor module self check abort:can not reach preload position\n");
							pthread_mutex_lock(&mutex_info);
							motor_module_check_info.motor_module_check_results = unreachable_zero_position;
							pthread_mutex_unlock(&mutex_info);
							is_check = 1;
							sem_post(&sem_client);
							break;
						}

						int32_t init_force_temp[10];
						uint32_t init_position_temp[10],init_position_overrange;

						motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);

						memcpy(init_force_temp,init_force+1,sizeof(init_force)-4);
						printf("init_temp = ");
						for(i=0;i<10;i++){
							printf("%d ",init_force_temp[i]);
						}
						printf("\n");
						memcpy(init_force,init_force_temp,sizeof(init_force)-4);

						pthread_mutex_lock(&mutex_force);			//互斥锁
						force_temp = force_now;
						pthread_mutex_unlock(&mutex_force);

						init_force[9] = SELF_CHECK_FORCE_VALUE - force_temp;
						printf("init_force = ");
						for(i=0;i<10;i++){
							printf("%d ",init_force[i]);
						}
						printf("\n");


						memcpy(init_position_temp,init_position+1,sizeof(init_position)-4);
						memcpy(init_position,init_position_temp,sizeof(init_position)-4);
						init_position[9] = motor_position.temp;
						printf("init_position = ");
						for(i=0;i<10;i++){
							printf("%d ",init_position[i]);
						}
						printf("\n");

						int init_ret;
						init_ret = 1;
						for(i=0;i<10;i++){
							if((abs(init_force[i]) - 25) < 0){
								init_ret = init_ret&1;
							}else{
								init_ret = init_ret&0;
							}
						}

						if(!init_ret){

							motor_cmd_position = motor_position.temp - MOTOR_ENCODER_DIRECTION*init_force[9]*100;
							printf("what is motor_cmd_position = %d\n",motor_cmd_position);
							motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
							motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);
							printf("what is init_force = %d\n",init_force[9]);

						}else{
							motor_ctl(TRAJECTORY_ABORT,NULL,NULL,MotorPort);
							for(i=0;i<10;i++){
								init_position_overrange = init_position_overrange + init_position[i];
							}
							motor_para_init_temp.preload_position = init_position_overrange/10;
							printf("what is preload_position = %d\n",motor_para_init_temp.preload_position);
							break;
						}
					}

					if(is_check == 1){
						break;
					}*/


                        //自检成功配置参数

                        motor_cmd_velocity = motor_para_init_temp.max_velocity;																		//设置运动速度为14000rpm 此参数需要可以配置
                        motor_ctl(SET_VELOCITY,&motor_cmd_velocity,NULL,MotorPort);

//					motor_cmd_position = motor_para_init_temp.zero_position;
                        motor_cmd_position = motor_para_init_temp.preload_position;
                        motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                        motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);

                        pthread_mutex_lock(&mutex_info);
                        sprintf(motor_module_check_info.motor_state,"%s",MOTOR_OK);
                        motor_module_check_info.motor_module_check_results = module_check_success;
                        pthread_mutex_unlock(&mutex_info);

                        is_check = 1;
                        sem_post(&sem_client);
                        printf("this is a test start\n");
                    }else if((is_check == 1)&&(motor_en_flag_temp_old!=CTL_CMDINITIAL)){
                        is_check = 0;
                    }

                    break;

                case CTL_CMDPOWERDOWN:																				//关机

                    nwrite = DISABLE_MOTOR;
                    motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);
                    sem_post(&sem_client);
                    return;

                    break;

                case CTL_CMDMOTIONSLEEP:																		//停机

                    if(motor_en_flag_temp != motor_en_flag_temp_old){

                        nwrite = ENABLE_POSITION_MODE;
                        motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);

                        motor_cmd_position = motor_para_init_temp.zero_position;
                        motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                        motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);
                        sem_post(&sem_client);
                    }

                    break;

                case CTL_CMDMOTIONSTOP:																						//停止

                    if(motor_en_flag_temp != motor_en_flag_temp_old){
                        nwrite = DISABLE_MOTOR;
                        motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);
                        sem_post(&sem_client);
                    }
                    break;

                case CTL_CMDMOTIONSTART:																					//开始工作
                    if(motor_en_flag_temp != motor_en_flag_temp_old){
                        nwrite = ENABLE_POSITION_MODE;
                        motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);
                        sem_post(&sem_client);
                    }
                    pthread_mutex_lock(&mutex_gait_msg);					//获取电机运动状态，socket
                    state_temp = state_now;
                    pthread_mutex_unlock(&mutex_gait_msg);

                    switch(state_temp){
                        case 01:																//预紧点，不能是单个位置点，需要在合适的步态和力矩下开始运动
                            if(state_temp != state_old){

                                integral_force = 0;

                                nwrite = ENABLE_POSITION_MODE;
                                motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);

                                motor_cmd_position = motor_para_init_temp.preload_position;
                                motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                                motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);
                            }
                            motor_ctl(GET_CURRENT,NULL,&motor_speed,MotorPort);
                            motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);
                            gettimeofday(&tv,NULL);
                            time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
                            time_now = (time_now - time_mark)&0x000fffff;

                            pthread_mutex_lock(&mutex_force);			//获取当前力矩
                            force_temp = force_now;
                            pthread_mutex_unlock(&mutex_force);

                            //printf("%u  motor_position = %d motor_speed = %d\n",time_now,motor_position.temp,motor_speed.temp);
                            printf("time=%u force=%d position=%d\n",time_now,force_temp,motor_position.temp);//time=0 force=123 position=0
                            break;

                        case 02:																//拉扯阶段，此阶段需要快速。因此将此阶段分为两段，一段是直接快速运动，当靠近最大位置时再引入力矩环

#if(GAIT_B_MODE==PULL_FORCE_TORQUE)

                            if(state_temp != state_old){

                                motor_ctl(TRAJECTORY_ABORT,NULL,NULL,MotorPort);

                                nwrite = ENABLE_VELOCITY_MODE;
                                motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);
                            }

                            motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);
//					motor_ctl(GET_ACTUAL_SPEED,NULL,&motor_speed,MotorPort);
                            gettimeofday(&tv,NULL);
                            time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
                            time_now = (time_now - time_mark)&0x000fffff;
														
														printf("where are you %u\n",time_now);
														
                            pthread_mutex_lock(&mutex_force);			//获取当前力矩
                            force_temp = force_now;
                            pthread_mutex_unlock(&mutex_force);

                            deltav_force = motor_para_init_temp.max_force - force_temp;

                            if(deltav_motor > motor_para_init_temp.pid_umax){
                                if(abs(deltav_force) > 200){
                                    index = 0;
                                }else{
                                    index = 1;
                                    if(deltav_force < 0){
                                        integral_force = integral_force + deltav_force;
                                    }
                                }
                            }else if(deltav_motor < motor_para_init_temp.pid_umin){
                                if(abs(deltav_force) > 200){
                                    index = 0;
                                }else{
                                    index = 1;
                                    if(deltav_force > 0){
                                        integral_force = integral_force + deltav_force;
                                    }
                                }
                            }else{
                                if(abs(deltav_force) > 200){
                                    index = 0;
                                }else{
                                    index = 1;
                                    integral_force = integral_force + deltav_force;
                                }
                            }

                            if(abs(deltav_force) < 10){
                                pndex = 0;
                            }else{
                                pndex = (int)motor_para_init_temp.pid_kp;	//1000
                            }

                            index = (int)motor_para_init_temp.pid_ki*index;

                            derivative_force = deltav_force_old - deltav_force;

                            motor_speed_t = 0-MOTOR_ENCODER_DIRECTION*(deltav_force*pndex + index*integral_force - derivative_force*0);

                            if(motor_speed_t > VELOCITY_MODE_MAX_SPEED)
                                motor_speed_t = VELOCITY_MODE_MAX_SPEED;
                            if(motor_speed_t < -VELOCITY_MODE_MAX_SPEED)
                                motor_speed_t = -VELOCITY_MODE_MAX_SPEED;

                            if(motor_position.temp < motor_para_init_temp.max_position - 4000 ){
                                if(motor_speed_t  < 0){
                                    motor_speed_t = 0;
                                }
                            }

                            motor_ctl(SET_VELOCITY_MODE_SPEED,&motor_speed_t,NULL,MotorPort);

                            deltav_force_old = deltav_force;
                            printf("time=%u force=%d position=%d derivative_force = %d\n",time_now,force_temp,motor_position.temp,derivative_force);//time=0 force=123 position=0
                            break;

#endif // (GAIT_B_MODE==PULL_FORCE_TORQUE)

#if(GAIT_B_MODE==PULL_FIX_POSITION)

                            if(state_temp != state_old){
                                motor_cmd_position = max_position;
                                motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                                motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);
                            }

                            motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);
                            gettimeofday(&tv,NULL);
                            time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
                            time_now = (time_now - time_mark)&0x000fffff;

                            pthread_mutex_lock(&mutex_force);			//获取当前力矩
							force_temp = force_now;
							pthread_mutex_unlock(&mutex_force);

							if(abs(motor_position.temp - motor_cmd_position) < 100){

							    max_force = max_force + force_temp;
							    max_force_cnt++;
							}
							printf("time=%u force=%d position=%d\n",time_now,force_temp,motor_position.temp);
							break;

#endif // (GAIT_B_MODE==PULL_FIX_POSITION)

                        case 03:					//回归零点，这个阶段就是快速就够了
                            if(state_temp != state_old){

                                integral_force = 0;

                                nwrite = ENABLE_POSITION_MODE;
                                motor_ctl(SET_DESIRED_STATE,&nwrite,NULL,MotorPort);

                                motor_cmd_position = motor_para_init_temp.zero_position;
                                motor_ctl(SET_MOTION,&motor_cmd_position,NULL,MotorPort);
                                motor_ctl(TRAJECTORY_MOVE,NULL,NULL,MotorPort);

                                motor_ctl(GET_MOTOR_FUALT,NULL,&motor_state,MotorPort);
                                pthread_mutex_lock(&mutex_info);
                                motor_module_run_info.motor_driver_state = (uint32_t)motor_state.temp;		//获取驱动器的状态，每个步态周期一次，0为全部正常，512为过流报警
                                pthread_mutex_unlock(&mutex_info);

                            }

                            pthread_mutex_lock(&mutex_force);			//获取当前力矩
                            force_temp = force_now;
                            pthread_mutex_unlock(&mutex_force);

                            motor_ctl(GET_ACTUAL_SPEED,NULL,&motor_speed,MotorPort);
                            motor_ctl(GET_POSITION,NULL,&motor_position,MotorPort);
                            gettimeofday(&tv,NULL);
                            time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
                            time_now = (time_now - time_mark)&0x000fffff;
                            //printf("%u  motor_position = %d motor_speed = %d\n",time_now,motor_position.temp,motor_speed.temp);
                            printf("time=%u force=%d position=%d\n",time_now,force_temp,motor_position.temp);//time=0 force=123 position=0
                            break;
                        default:
                            usleep(10000);
                            break;
                    }

#if(GAIT_B_MODE==PULL_FIX_POSITION)

                    if((state_temp == 1)&&(state_old == 3)&&(max_force_cnt != 0)){

                        gettimeofday(&tv,NULL);
                        time_now = (uint32_t)(tv.tv_sec*1000+tv.tv_usec/1000);
                        time_now = (time_now - time_mark)&0x000fffff;

                        max_force = (uint32_t)(max_force/max_force_cnt);
                        deltav_force = motor_para_init_temp.max_force - max_force;
                        max_position = max_position - MOTOR_ENCODER_DIRECTION*deltav_force*2;

                        if(max_position < motor_para_init_temp.max_position - 4000){
                            max_position = motor_para_init_temp.max_position - 4000;
                        }else if(max_position > motor_para_init_temp.max_position + 4000){
                            max_position = motor_para_init_temp.max_position + 4000;
                        }
                        max_force = 0;
                        max_force_cnt = 0;
                    }

#endif // (GAIT_B_MODE==PULL_FIX_POSITION)
                    state_old = state_temp;
                    break;
                default:
                    usleep(100000);
                    break;
            }
            motor_en_flag_temp_old = motor_en_flag_temp;
        }
    }
    close(MotorPort);
}


char* zeromq_msg_getdata(char* msg,char *type,uint8_t len)
{
    char *p,*s;
    char temp[256];

    p = strstr(msg,type);
    memset(temp,0,sizeof(temp));
    s = temp;

    if(p != NULL){
        p = p + len;
        while((*p != 0x20)&&(*p != 0)){
            if((uint16_t*)p < (uint16_t*)msg + 1024){
                *s = *p;
                s++;
                p++;
            }else{
                return NULL;
            }
        }
        s =temp;
        return s;
    }
    return NULL;
}


//socket通讯线程，处理获取到的数据。
void thread_gait_zeromq(void)
{
    uint32_t state_temp;

    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
//	zmq_connect (requester, "tcp://localhost:8011");
    zmq_connect (requester, "tcp://192.168.1.8:8011");
//	zmq_connect (requester, "tcp://192.168.1.14:8011");
//	zmq_connect (requester, "tcp://192.168.1.11:8011");
    zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    int request_nbr,zmq_gait_try = 1024;
    char* s;


    while (zmq_gait_try) {
        char buffer[1024],temp[32];
        memset(buffer,0,sizeof(buffer));
        zmq_recv (requester, buffer, sizeof(buffer), 0);
//		printf("what is rev : %s\n",buffer);
        s = zeromq_msg_getdata(buffer,GAIT,sizeof(GAIT));
        if(s != NULL){

            if(*s == 0x41)				//"B"
                state_temp = 1;
            else if(*s == 0x42)		//"C"
                state_temp = 2;
            else if(*s == 0x43)		//"A"
                state_temp = 3;
//			else if(*s == 0x4E)
//				state_temp = 3;

            pthread_mutex_lock(&mutex_gait_msg);
            state_now = state_temp;
            pthread_mutex_unlock(&mutex_gait_msg);
            zmq_gait_try = 1024;
        } else{
            zmq_gait_try--;
        }
//		s = zeromq_msg_getdata(buffer,FORCE,sizeof(FORCE));
//		if(s != NULL){
//			sprintf(temp,"%s",s);
//			printf("Received state is %s\n",temp);
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);
}

int get_default_settings(void)
{
    int ret;
    void *handle;
    const char *filepath = "./motor_para_defaults.txt";
    struct motor_para_init_t motor_para_init_temp;

    ret = init(filepath, &handle);
    if(ret != 0){
        return -1;
    }

    char valuebuf[128];

    ret = getValue(handle, "max_force", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.max_force);
    }

    ret = getValue(handle, "max_position", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.max_position);
    }

    ret = getValue(handle, "zero_position", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.zero_position);
    }

    ret = getValue(handle, "preload_position", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.preload_position);
    }

    ret = getValue(handle, "max_velocity", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.max_velocity);
    }

    ret = getValue(handle, "nset_acc", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.nset_acc);
    }

    ret = getValue(handle, "max_pot", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%f",&motor_para_init_temp.max_pot);
    }

    ret = getValue(handle, "pid_kp", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%f",&motor_para_init_temp.pid_kp);
    }

    ret = getValue(handle, "pid_ki", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%f",&motor_para_init_temp.pid_ki);
    }

    ret = getValue(handle, "pid_umax", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.pid_umax);
    }

    ret = getValue(handle, "pid_umin", valuebuf);
    if (ret != 0) {
        return -1;
    }else{
        sscanf(valuebuf,"%d",&motor_para_init_temp.pid_umin);
    }

    motor_para_init = motor_para_init_temp;

    ret = release(&handle);
    if (ret != 0) {
        return -1;
    }

    return 0;
}


struct timespec gettimeout(uint32_t timeout)
{
    struct timeval tv;
    struct timespec ts;

    gettimeofday(&tv,NULL);
    ts.tv_sec = tv.tv_sec + timeout;
//	ts.tv_nsec = tv.tv_usec*1000 + timeout*1000*1000;
//	ts.tv_sec += ts.tv_nsec/(1000*1000*1000);
//	ts.tv_nsec %= (1000*1000*1000);
    return ts;
}

//与系统管理模块通讯
int thread_client_zeromq(void)
{


    CTLCmdMsgTypeDef cmdmotormsg;
    memset(&cmdmotormsg,0,sizeof(cmdmotormsg));

    CTLCmdRtMsgTypeDef sendrdymsg;
    memset(&sendrdymsg,0,sizeof(sendrdymsg));
    sendrdymsg.nodeID = NODEID_OF_MOTOR;

    struct timespec ts;
    uint32_t timeout = 1000;

    char buffer[256] = "",rawdata[256]="",forceaid_str[32];
    char *s;
    int forceaid_temp = 3,zmq_client_try = 1024;
    int32_t motor_en_flag_temp;

//    pthread_mutex_lock(&mutex_client_msg);
//	get_default_settings();
//    pthread_mutex_unlock(&mutex_client_msg);

    motor_en_flag_temp = CTL_CMDINITIAL;
    pthread_mutex_lock(&mutex_client_msg);
    get_default_settings();
    motor_en_flag = motor_en_flag_temp;
    pthread_mutex_unlock(&mutex_client_msg);

    sem_post(&sem_motor);
    sem_wait(&sem_client);

    sleep(2);

    if(motor_module_check_info.motor_module_check_results == module_check_success){
        motor_en_flag_temp = CTL_CMDMOTIONSTART;
        pthread_mutex_lock(&mutex_client_msg);
        motor_en_flag = motor_en_flag_temp;
        pthread_mutex_unlock(&mutex_client_msg);
        sprintf(rawdata,"initialsuccess");
    }else{
        sprintf(rawdata,"initialerrorID:%d",motor_module_check_info.motor_module_check_results);
    }

    //  Socket to talk to clients
    void *context = zmq_ctx_new ();
    void *responder = zmq_socket (context, ZMQ_REP);
    int rc = zmq_bind (responder,"tcp://*:8001");
//	printf("what is rc %d\n",rc);
    assert (rc == 0);


    while(zmq_client_try){

        zmq_recv(responder,buffer,sizeof(buffer),0);
        printf("what is rev:%s\n",buffer);

        if(strstr(buffer,"cmdmotorintial")!=NULL){
            if(motor_en_flag_temp != CTL_CMDINITIAL){
                motor_en_flag_temp = CTL_CMDINITIAL;
                pthread_mutex_lock(&mutex_client_msg);
                motor_en_flag = motor_en_flag_temp;
                pthread_mutex_unlock(&mutex_client_msg);

                timeout = 120;																	//秒 sec
                ts = gettimeout(timeout);

                sem_post(&sem_motor);

                if(sem_timedwait(&sem_client,&ts) == 0){
                    pthread_mutex_lock(&mutex_info);
                    if(motor_module_check_info.motor_module_check_results == module_check_success){
                        sprintf(rawdata,"initialsuccess");
                    }else{
                        sprintf(rawdata,"initialerrorID:%d",motor_module_check_info.motor_module_check_results);
                    }
                    pthread_mutex_unlock(&mutex_info);

                }else{
                    sprintf(rawdata,"initialerrorID");
                }
            } else{
                sprintf(rawdata,"initialsuccess");
            }

            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorshutdown")!=NULL){
            if(motor_en_flag_temp != CTL_CMDPOWERDOWN){
                motor_en_flag_temp = CTL_CMDPOWERDOWN;
                pthread_mutex_lock(&mutex_client_msg);
                motor_en_flag = motor_en_flag_temp;
                pthread_mutex_unlock(&mutex_client_msg);

                timeout = 10;
                ts = gettimeout(timeout);

                sem_post(&sem_motor);

                if(sem_timedwait(&sem_client,&ts)==0){
                    sprintf(rawdata,"shutdownsuccess");
                }else{
                    sprintf(rawdata,"shutdownerrorID");
                }
            } else{
                sprintf(rawdata,"shutdownsuccess");
            }
            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorstop")!=NULL){
            if(motor_en_flag_temp != CTL_CMDMOTIONSTOP){
                motor_en_flag_temp = CTL_CMDMOTIONSTOP;
                pthread_mutex_lock(&mutex_client_msg);
                motor_en_flag = motor_en_flag_temp;
                pthread_mutex_unlock(&mutex_client_msg);

                timeout = 10;
                ts = gettimeout(timeout);

                sem_post(&sem_motor);

                if(sem_timedwait(&sem_client,&ts) == 0){
                    sprintf(rawdata,"stopsuccess");
                }else{
                    sprintf(rawdata,"stoperrorID");
                }
            } else{
                sprintf(rawdata,"stopsuccess");
            }

            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorpause")!=NULL){

            if(motor_en_flag_temp != CTL_CMDMOTIONSLEEP){
                motor_en_flag_temp = CTL_CMDMOTIONSLEEP;
                pthread_mutex_lock(&mutex_client_msg);
                motor_en_flag = motor_en_flag_temp;
                pthread_mutex_unlock(&mutex_client_msg);

                timeout = 10;
                ts = gettimeout(timeout);

                sem_post(&sem_motor);

                if(sem_timedwait(&sem_client,&ts) == 0){
                    sprintf(rawdata,"pausesuccess");
                }else{
                    sprintf(rawdata,"pauseerrorID");
                }
            }else{
                sprintf(rawdata,"pausesuccess");
            }

            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorstart")!=NULL){
            if(motor_en_flag_temp != CTL_CMDMOTIONSTART){
                motor_en_flag_temp = CTL_CMDMOTIONSTART;
                pthread_mutex_lock(&mutex_client_msg);
                motor_en_flag = motor_en_flag_temp;
                pthread_mutex_unlock(&mutex_client_msg);

                timeout = 10;
                ts = gettimeout(timeout);

                sem_post(&sem_motor);

                if(sem_timedwait(&sem_client,&ts) == 0){
                    sprintf(rawdata,"startsuccess");
                }else{
                    sprintf(rawdata,"starterrorID");
                }
            }else{
                sprintf(rawdata,"startsuccess");
            }
            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorsetparam")!=NULL){

            sprintf(rawdata,"setparamsuccess");
            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        }else if(strstr(buffer,"cmdmotorforceaid")!=NULL){

            s = zeromq_msg_getdata(buffer,"cmdmotorforceaid",sizeof("cmdmotorforceaid"));
            if(s!=NULL){
                sprintf(forceaid_str,"%s",s);
                sscanf(forceaid_str,"%d",&forceaid_temp);
            }

            pthread_mutex_lock(&mutex_client_msg);

            pthread_mutex_unlock(&mutex_client_msg);
            sprintf(rawdata,"setforceaidsuccess");
            zmq_client_try = 1024;

            zmq_send(responder,rawdata,strlen(rawdata),0);
            printf("zmq send ok!  %s\n",rawdata);
        } else{
            zmq_client_try--;
        }

    }


    zmq_close (responder);
    zmq_ctx_destroy (context);
    return 0;
}




void thread_motor_module_run_info(void)
{
    struct motor_module_run_info_t motor_module_run_info_temp;
    struct motor_module_check_info_t motor_module_check_info_temp;
    uint32_t force_temp;
    float pot_temp;
    char rawdata[256];

    //  Prepare our context and publisher
    void *context = zmq_ctx_new ();
    void *publisher = zmq_socket (context, ZMQ_PUB);
    int rc = zmq_bind (publisher, "tcp://*:8025");
    assert (rc == 0);

    while(1){

        pthread_mutex_lock(&mutex_info);
        motor_module_run_info_temp = motor_module_run_info;
        motor_module_check_info_temp = motor_module_check_info;
        pthread_mutex_unlock(&mutex_info);

        pthread_mutex_lock(&mutex_force);			//获取当前力矩
        force_temp = force_now;
        pthread_mutex_unlock(&mutex_force);

        pthread_mutex_lock(&mutex_pot);			//获取当前力矩
        pot_temp = pot_now;
        pthread_mutex_unlock(&mutex_pot);

        sprintf(rawdata,"motor_module_check_info##motor_state:%s,force_state:%s,pot_state:%s##",motor_module_check_info_temp.motor_state,motor_module_check_info_temp.force_port_state,motor_module_check_info_temp.pot_port_state);

        zmq_send(publisher,rawdata,strlen(rawdata),0);

        usleep(100000);
    }

    zmq_close (publisher);
    zmq_ctx_destroy (context);
}


void thread_test_press(void)
{

    void *context = zmq_ctx_new ();
    void *requester = zmq_socket (context, ZMQ_SUB);
    zmq_connect (requester, "tcp://192.168.1.8:8033");
//	zmq_connect (requester, "tcp://192.168.1.14:8011");
//	zmq_connect (requester, "tcp://192.168.1.11:8011");
    zmq_setsockopt(requester, ZMQ_SUBSCRIBE, "", 0);
    char* s;

    while (1) {
        char buffer[1024],temp[32];
        memset(buffer,0,sizeof(buffer));
        zmq_recv (requester, buffer, sizeof(buffer), 0);
        s = strstr(buffer,"p1");
        if(s!=NULL){
            int s_len = strlen(s);
            pthread_mutex_lock(&mutex_foot_test);
            memcpy(foot_test,s,s_len);
            pthread_mutex_unlock(&mutex_foot_test);
        }
    }
    zmq_close (requester);
    zmq_ctx_destroy (context);

}