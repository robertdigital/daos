//
// (C) Copyright 2019 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package server

import (
	"context"
	"sync"
	"time"

	"github.com/golang/protobuf/proto"

	"github.com/daos-stack/daos/src/control/drpc"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/server/ioserver"
)

// Utilities for internal server package tests

// mockDrpcClientConfig is a configuration structure for mockDrpcClient
type mockDrpcClientConfig struct {
	IsConnectedBool bool
	ConnectError    error
	CloseError      error
	SendMsgResponse *drpc.Response
	SendMsgError    error
	ResponseDelay   time.Duration
}

func (cfg *mockDrpcClientConfig) setSendMsgResponse(status drpc.Status, body []byte, err error) {
	cfg.SendMsgResponse = &drpc.Response{
		Status: status,
		Body:   body,
	}
	cfg.SendMsgError = err
}

func (cfg *mockDrpcClientConfig) setResponseDelay(duration time.Duration) {
	cfg.ResponseDelay = duration
}

// mockDrpcClient is a mock of the DomainSocketClient interface
type mockDrpcClient struct {
	sync.Mutex
	cfg              mockDrpcClientConfig
	CloseCallCount   int
	SendMsgInputCall *drpc.Call
}

func (c *mockDrpcClient) IsConnected() bool {
	return c.cfg.IsConnectedBool
}

func (c *mockDrpcClient) Connect() error {
	return c.cfg.ConnectError
}

func (c *mockDrpcClient) Close() error {
	c.CloseCallCount++
	return c.cfg.CloseError
}

func (c *mockDrpcClient) SendMsg(call *drpc.Call) (*drpc.Response, error) {
	c.SendMsgInputCall = call

	<-time.After(c.cfg.ResponseDelay)

	return c.cfg.SendMsgResponse, c.cfg.SendMsgError
}

func newMockDrpcClient(cfg *mockDrpcClientConfig) *mockDrpcClient {
	if cfg == nil {
		cfg = &mockDrpcClientConfig{}
	}

	return &mockDrpcClient{cfg: *cfg}
}

// setupMockDrpcClientBytes sets up the dRPC client for the mgmtSvc to return
// a set of bytes as a response.
func setupMockDrpcClientBytes(svc *mgmtSvc, respBytes []byte, err error) {
	mi, _ := svc.harness.GetMSLeaderInstance()

	cfg := &mockDrpcClientConfig{}
	cfg.setSendMsgResponse(drpc.Status_SUCCESS, respBytes, err)
	mi.setDrpcClient(newMockDrpcClient(cfg))
}

// setupMockDrpcClient sets up the dRPC client for the mgmtSvc to return
// a valid protobuf message as a response.
func setupMockDrpcClient(svc *mgmtSvc, resp proto.Message, err error) {
	respBytes, _ := proto.Marshal(resp)
	setupMockDrpcClientBytes(svc, respBytes, err)
}

// newTestIOServer returns an IOServerInstance configured for testing.
func newTestIOServer(log logging.Logger, isAP bool) *IOServerInstance {
	r := ioserver.NewTestRunner(nil, ioserver.NewConfig())

	var msCfg mgmtSvcClientCfg
	if isAP {
		msCfg.AccessPoints = append(msCfg.AccessPoints, "localhost")
	}

	srv := NewIOServerInstance(log, nil, nil, newMgmtSvcClient(context.TODO(), log, msCfg), r)
	srv.setSuperblock(&Superblock{
		MS: isAP,
	})

	return srv
}

// newTestMgmtSvc creates a mgmtSvc that contains an IOServerInstance
// properly set up as an MS.
func newTestMgmtSvc(log logging.Logger) *mgmtSvc {
	srv := newTestIOServer(log, true)

	harness := NewIOServerHarness(log)
	if err := harness.AddInstance(srv); err != nil {
		panic(err)
	}

	return newMgmtSvc(harness, nil)
}

// newTestMgmtSvcMulti creates a mgmtSvc that contains the requested
// number of IOServerInstances. If requested, the first instance is
// configured as an access point.
func newTestMgmtSvcMulti(log logging.Logger, count int, isAP bool) *mgmtSvc {
	harness := NewIOServerHarness(log)

	for i := 0; i < count; i++ {
		srv := newTestIOServer(log, i == 0 && isAP)

		if err := harness.AddInstance(srv); err != nil {
			panic(err)
		}
	}

	return newMgmtSvc(harness, nil)
}
