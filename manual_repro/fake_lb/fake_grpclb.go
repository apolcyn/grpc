// Runs a fake grpclb server for tests
package main

import (
	"encoding/binary"
	"flag"
	"net"
	"strconv"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/alts"
	"google.golang.org/grpc/grpclb/grpc_lb_v1/messages"
	lbpb "google.golang.org/grpc/grpclb/grpc_lb_v1/service"
	"google.golang.org/grpc/grpclog"
)

const localhostIpv4 = 0x7f000001 // 127.0.0.1

var (
	port        = flag.Int("port", 10000, "The server port")
	backendPort = flag.Int("backend_port", 20000, "The backend server port")
	debugMode   = flag.Bool("debug_mode", false, "Run the server with insecure credentials")
)

type loadBalancerServer struct{}

func (*loadBalancerServer) BalanceLoad(stream lbpb.LoadBalancer_BalanceLoadServer) error {
	grpclog.Info("Begin handling new BalancerLoad request.")
	if _, err := stream.Recv(); err != nil {
		grpclog.Errorf("Error receiving LoadBalanceRequest: %v", err)
		return err
	}
	grpclog.Info("LoadBalancerRequest received.")
	localhostIpv4Bytes := make([]byte, 4, 4)
	binary.BigEndian.PutUint32(localhostIpv4Bytes, localhostIpv4)
	res := &messages.LoadBalanceResponse{
		LoadBalanceResponseType: &messages.LoadBalanceResponse_ServerList{
			ServerList: &messages.ServerList{
				Servers: []*messages.Server{
					{
						IpAddress: localhostIpv4Bytes,
						Port:      int32(*backendPort),
					},
				},
			},
		},
	}
	stream.Send(res)
	grpclog.Info("Sent LoadBalanceResponse")
	return nil
}

func main() {
	// Use flag.Parse() instead of google.init():
	//
	// Though this .go file is checked in to piper, this test server runs
	// inside of a docker container, within a GCE VM instance, and it
	// compiles against open source grpc and other go libraries,
	// I.e. it's meant for an open source environment, so it can't
	// use anything internal to google.
	flag.Parse()
	var opts []grpc.ServerOption
	if !*debugMode {
		altsCreds := grpc.Creds(alts.NewServerCreds())
		opts = append(opts, altsCreds)
	}
	server := grpc.NewServer(opts...)
	grpclog.Infof("Begin listening on %d.", *port)
	lis, _ := net.Listen("tcp", ":"+strconv.Itoa(*port))
	lbpb.RegisterLoadBalancerServer(server, &loadBalancerServer{})
	server.Serve(lis)
}
