package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"net"
	"strconv"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/alts"
	"google.golang.org/grpc/grpclb/grpc_lb_v1/messages"
	lbpb "google.golang.org/grpc/grpclb/grpc_lb_v1/service"
	"google.golang.org/grpc/grpclog"
)

var (
	port         = flag.Int("port", 10000, "The server port")
	backend_port = flag.Int("backend_port", 20000, "The backend server port")
        insecure     = flag.Bool("insecure", false, "insecure")
)

type loadBalancerServer struct{}

func (*loadBalancerServer) BalanceLoad(stream lbpb.LoadBalancer_BalanceLoadServer) error {
	grpclog.Info("Begin handling new BalancerLoad request.")
	if _, err := stream.Recv(); err != nil {
		grpclog.Errorf("Error receiving LoadBalanceRequest: %v", err)
		return err
	}
	grpclog.Info("LoadBalancerRequest received.")
	localhost_ipv4 := make([]byte, 4, 4)
	binary.BigEndian.PutUint32(localhost_ipv4, 0x7f000001)
	res := &messages.LoadBalanceResponse{
		LoadBalanceResponseType: &messages.LoadBalanceResponse_ServerList{
			ServerList: &messages.ServerList{
				Servers: []*messages.Server{
					{
						IpAddress: localhost_ipv4,
						Port:      int32(*backend_port),
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
	flag.Parse()
	var opts []grpc.ServerOption
        if !*insecure {
	        altsCreds := grpc.Creds(alts.NewServerCreds())
	        opts = append(opts, altsCreds)
        }
	server := grpc.NewServer(opts...)
	fmt.Printf("Begin listing on %d with ALTS credentials.\n", *port)
	lis, _ := net.Listen("tcp", ":"+strconv.Itoa(*port))
	lbpb.RegisterLoadBalancerServer(server, &loadBalancerServer{})
	server.Serve(lis)
}
