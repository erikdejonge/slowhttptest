/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * *****************************************************************************/

/*****
 * Author: Sergey Shekyan sshekyan@qualys.com
 *
 * Slow HTTP attack  vulnerability test tool
 *  http://code.google.com/p/slowhttptest/
 *****/

#include "slowhttptest.h"

#include <errno.h>
#include <cmath>
#include <stdio.h>

#include <string>
#include <vector>

#include <netdb.h>
#include <sys/time.h>

#include "slowlog.h"
#include "slowsocket.h"
#include "slowhttptest.h"

#define BUF_SIZE 65537
#define USER_AGENT "Mozilla/5.0 (Macintosh; U; Intel Mac OS X 10_6_8; en-us) \
 AppleWebKit/533.21.1 (KHTML, like Gecko) Version/5.0.5 Safari/533.21.1"

static const char post_request[] = "Connection: close\r\n"
		"Referer: http://code.google.com/p/slowhttptest/r\n"
		"Content-Type: application/x-www-form-urlencoded\r\n"
		"Content-Length: 512\r\n"
		"Accept: text/html;q=0.9,text/plain;q=0.8,image/png,*/*;q=0.5\r\n\r\n"
		"foo=bar";

static const char post_extra[] = "alpha=beta&";

static const char header_extra[] = "X-Header: 1234567\r\n";

static const char symbols[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

SlowHTTPTest::SlowHTTPTest(int delay, int duration, int interval,
 int con_cnt, SlowTestType type) :
  delay_(delay)
  ,duration_(duration)
  ,followup_timing_(interval)
  ,followup_cnt_(duration_ / followup_timing_)
  ,num_connections_(con_cnt)
  ,type_(type)
{
}

SlowHTTPTest::~SlowHTTPTest() {
}

bool SlowHTTPTest::fillRandomData(char * random_string, const size_t len) {
	if(len > 0) {
		size_t pos = 0;
		while(pos < len) {
			random_string[pos++] = symbols[rand() % 51];
		}

		random_string[len] = '\0';
		return true;
	} else
		return false;
}

bool SlowHTTPTest::init(const char* url) {
	
		if(!base_uri_.prepare(url)) {
		return false;
  }
	server_ = gethostbyname(base_uri_.getHost().c_str());
	if(server_ == NULL) {
		printf("%s: ERROR, no such host\n", __FUNCTION__);
		return false;
	}
	user_agent_.append(USER_AGENT);
	request_.clear();
	if(eHeader == type_) {
		request_.append("GET ");
	} else {
		request_.append("POST ");
	}

	request_.append(base_uri_.getPath());
	request_.append(" HTTP/1.1\r\n");
	request_.append("Host: ");
	request_.append(base_uri_.getHost());

	if(base_uri_.getPort() != 80 || base_uri_.getPort() != 443) {
		request_.append(":");
		char buf[4];
		sprintf(buf, "%d", base_uri_.getPort());
		request_.append(buf);
	}

	request_.append("\r\n");
	request_.append("User-Agent: ");
	request_.append(user_agent_);
	request_.append("\r\n");
	if(ePost == type_) {
		request_.append(post_request);
  }
  report_parameters();
	return true;
}

bool SlowHTTPTest::grabResponseCode(const char* buf, unsigned int& code) {

	if(!buf) return false;

	char *s = strstr(buf, "HTTP/");
	if(s) {
		while(*s && !isspace(*s))
			++s;
		char* e = s;
		while(*e && isspace(*e))
			++e;
		s = e;
		while(*e && isdigit(*e))
			++e;
		char codeStr[4];
		strncpy(codeStr, s, e - s);
		codeStr[3] = '\0';
		code = atoi(codeStr);
	}
	if(code)
		return true;
	else
		return false;
}

void SlowHTTPTest::report_parameters() {

  slowlog("\nUsing:\n"
    "test mode:                        %s\n"
    "URL:                              %s\n"
    "number of connections:            %d\n"
    "interval between follow up data:  %d seconds\n"
    "connections per seconds:          %d\n"
    "test duration:                    %d seconds\n",
     type_?"POST":"headers"
    , base_uri_.getData()
    , num_connections_
    , followup_timing_
    , delay_
    , duration_
  );
}

void SlowHTTPTest::remove_sock(int id) {
	delete sock_[id];
	sock_[id] = 0;
}

bool SlowHTTPTest::run_test() {
	size_t num_connected = 0;
	fd_set readfds, writefds;
	int maxfd = 0;
	int result = 0;
	int ret = 0;
	int last_followup_timing = 0;
	timeval now, timeout, start, progressTimer;
	unsigned int seconds_passed = 0; //stores seconds passed since we started
	unsigned int active_sock_num;
	char buf[BUF_SIZE];
	const char* extra_data;
  unsigned int heartbeat_reported = 1; //trick to print 0 sec hb	
  timerclear(&now);
	timerclear(&timeout);
	timerclear(&progressTimer);
	gettimeofday(&start, 0);

	if(eHeader == type_) {
		extra_data = header_extra;
	} else {
		extra_data = post_extra;
	}

	sock_.resize(num_connections_);

	// select loop
	while(true) {
		//slowlog("%s:while loop %d, %d seconds_passed %d\n", __FUNCTION__,
		//		(int) active_sock_num, (int) sock_.size(), seconds_passed);
		int wr = 0;
		active_sock_num = 0;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0; //microseconds
		if(num_connected < num_connections_) {
			sock_[num_connected] = new SlowSocket();
			if(!sock_[num_connected]->init(server_, &base_uri_, maxfd,
					followup_cnt_)) {
				printf("%s: Unable to initialize %dth socket.\n", __FUNCTION__,
						(int) num_connected);
				num_connections_ = num_connected;
			} else {
				++num_connected;
				usleep(1000000 / delay_);
			}
		}
		seconds_passed = progressTimer.tv_sec;
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		for(int i = 0; i < num_connected; ++i) {
			if(sock_[i] && sock_[i]->get_sockfd() > 0) {
				FD_SET(sock_[i]->get_sockfd(), &readfds);
				++active_sock_num;
				if(sock_[i]->get_requests_to_send() > 0) {
					++wr;
          FD_SET(sock_[i]->get_sockfd(), &writefds);
				} else if(sock_[i]->get_followups_to_send() > 0
						&& (seconds_passed > 0 && seconds_passed % followup_timing_ == 0)) {
					if(last_followup_timing != seconds_passed) {
						last_followup_timing = seconds_passed;
						++wr;
            FD_SET(sock_[i]->get_sockfd(), &writefds);
					}
				}
			}
		}
		if(seconds_passed % 5 == 0) { //printing heartbeat
      if(heartbeat_reported != seconds_passed) { //not so precise
        printf("%s: Slow HTTP test status: %d open connection(s) on %dth second\n",
          __FUNCTION__, (int)active_sock_num, seconds_passed);
        heartbeat_reported = seconds_passed;
      }
      // more precise
      slowlog("%s: Slow HTTP test status: %d open connection(s) on %dth second\n",
        __FUNCTION__, (int)active_sock_num, seconds_passed);
		}
		if(seconds_passed > duration_ || active_sock_num == 0) { //limit to test
			break;
		}

		result = select(maxfd + 1, &readfds, wr ? &writefds : NULL, NULL
				, &timeout);
		gettimeofday(&now, 0);
		timersub(&now, &start, &progressTimer);
		if(result < 0) {
			printf("%s: select() error: %s\n", __FUNCTION__, strerror(errno));
			break;
		} else if(result == 0) {
			continue;
		} else {
			for(int i = 0; i < num_connected; i++) {
				if(sock_[i] && sock_[i]->get_sockfd() > 0) {
					if(FD_ISSET(sock_[i]->get_sockfd(), &readfds)) { //read
						ret = sock_[i]->recv_slow(buf, BUF_SIZE);
						buf[ret] = '\0';
						if(ret <= 0 && errno != EAGAIN) {
							slowlog("%s: sock %d closed\n", __FUNCTION__,
									sock_[i]->get_sockfd());
							remove_sock(i);
							continue;
						} else {
							slowlog("%s: sock %d replied %s\n", __FUNCTION__,
									sock_[i]->get_sockfd(), buf);
						}
					}
					if(FD_ISSET(sock_[i]->get_sockfd(), &writefds)) { //write
						if(sock_[i]->get_requests_to_send() > 0) {
							ret = sock_[i]->send_slow(request_.c_str(),
									request_.size());
							if(ret <= 0 && errno != EAGAIN) {
								slowlog(
										"%s:error sending initial slow post on sock %d: %s\n",
										__FUNCTION__, sock_[i]->get_sockfd(),
										strerror(errno));
								remove_sock(i);
								continue;
							} else {
								slowlog(
										"%s:initial %d of %d bytes sent on slow post socket %d:\n %s\n",
										__FUNCTION__, ret,
										(int) request_.size(),
										(int) sock_[i]->get_sockfd(),
										request_.c_str());
							}
						} else if(sock_[i]->get_followups_to_send() > 0
								&& (seconds_passed > 0
										&& seconds_passed % followup_timing_ == 0)) {
							ret = sock_[i]->send_slow(extra_data,
									strlen(extra_data), eFollowUpSend);
							if(ret <= 0 && errno != EAGAIN) {
								slowlog(
										"%s:error sending follow up data on socket %d: %s\n",
										__FUNCTION__, sock_[i]->get_sockfd(),
										strerror(errno));
								remove_sock(i);
								continue;
							} else {
								slowlog(
										"%s:%d of %d follow up data sent on socket %d:\n%s\n%d follow ups left\n",
										__FUNCTION__, ret,
										(int) strlen(extra_data),
										(int) sock_[i]->get_sockfd(),
										extra_data,
										sock_[i]->get_followups_to_send());
							}
						}
					} else {
            if(sock_[i] && sock_[i]->get_requests_to_send() > 0) {
              // trying to connect, server slowing down probably
              slowlog("pending connection on socket %d\n", sock_[i]->get_sockfd());
             // printf("pending connection on socket %d\n", sock_[i]->get_sockfd());
            }
          }
				}
			}
		}
	}

  printf("%d active sockets left by the end of the test on %dth second\n",
   active_sock_num, seconds_passed);
  slowlog("%s: %d active sockets left by the end of the test on %dth second\n",
   __FUNCTION__, active_sock_num, seconds_passed);
  for(int i = 0; i < num_connections_; ++i) {
    if(sock_[i]) {
      delete sock_[i];
		}
	}
	sock_.clear();

	return true;
}