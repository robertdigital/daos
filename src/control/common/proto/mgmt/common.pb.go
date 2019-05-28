// Code generated by protoc-gen-go. DO NOT EDIT.
// source: common.proto

package mgmt

import proto "github.com/golang/protobuf/proto"
import fmt "fmt"
import math "math"

// Reference imports to suppress errors if they are not otherwise used.
var _ = proto.Marshal
var _ = fmt.Errorf
var _ = math.Inf

// This is a compile-time assertion to ensure that this generated file
// is compatible with the proto package it is being compiled against.
// A compilation error at this line likely means your copy of the
// proto package needs to be updated.
const _ = proto.ProtoPackageIsVersion2 // please upgrade the proto package

type ResponseStatus int32

const (
	ResponseStatus_CTRL_SUCCESS     ResponseStatus = 0
	ResponseStatus_CTRL_IN_PROGRESS ResponseStatus = 1
	ResponseStatus_CTRL_WAITING     ResponseStatus = 2
	ResponseStatus_CTRL_ERR_CONF    ResponseStatus = -1
	ResponseStatus_CTRL_ERR_NVME    ResponseStatus = -2
	ResponseStatus_CTRL_ERR_SCM     ResponseStatus = -3
	ResponseStatus_CTRL_ERR_APP     ResponseStatus = -4
	ResponseStatus_CTRL_ERR_UNKNOWN ResponseStatus = -5
	ResponseStatus_CTRL_NO_IMPL     ResponseStatus = -6
)

var ResponseStatus_name = map[int32]string{
	0:  "CTRL_SUCCESS",
	1:  "CTRL_IN_PROGRESS",
	2:  "CTRL_WAITING",
	-1: "CTRL_ERR_CONF",
	-2: "CTRL_ERR_NVME",
	-3: "CTRL_ERR_SCM",
	-4: "CTRL_ERR_APP",
	-5: "CTRL_ERR_UNKNOWN",
	-6: "CTRL_NO_IMPL",
}
var ResponseStatus_value = map[string]int32{
	"CTRL_SUCCESS":     0,
	"CTRL_IN_PROGRESS": 1,
	"CTRL_WAITING":     2,
	"CTRL_ERR_CONF":    -1,
	"CTRL_ERR_NVME":    -2,
	"CTRL_ERR_SCM":     -3,
	"CTRL_ERR_APP":     -4,
	"CTRL_ERR_UNKNOWN": -5,
	"CTRL_NO_IMPL":     -6,
}

func (x ResponseStatus) String() string {
	return proto.EnumName(ResponseStatus_name, int32(x))
}
func (ResponseStatus) EnumDescriptor() ([]byte, []int) {
	return fileDescriptor_common_32774cfc86ee87c3, []int{0}
}

type EmptyParams struct {
	XXX_NoUnkeyedLiteral struct{} `json:"-"`
	XXX_unrecognized     []byte   `json:"-"`
	XXX_sizecache        int32    `json:"-"`
}

func (m *EmptyParams) Reset()         { *m = EmptyParams{} }
func (m *EmptyParams) String() string { return proto.CompactTextString(m) }
func (*EmptyParams) ProtoMessage()    {}
func (*EmptyParams) Descriptor() ([]byte, []int) {
	return fileDescriptor_common_32774cfc86ee87c3, []int{0}
}
func (m *EmptyParams) XXX_Unmarshal(b []byte) error {
	return xxx_messageInfo_EmptyParams.Unmarshal(m, b)
}
func (m *EmptyParams) XXX_Marshal(b []byte, deterministic bool) ([]byte, error) {
	return xxx_messageInfo_EmptyParams.Marshal(b, m, deterministic)
}
func (dst *EmptyParams) XXX_Merge(src proto.Message) {
	xxx_messageInfo_EmptyParams.Merge(dst, src)
}
func (m *EmptyParams) XXX_Size() int {
	return xxx_messageInfo_EmptyParams.Size(m)
}
func (m *EmptyParams) XXX_DiscardUnknown() {
	xxx_messageInfo_EmptyParams.DiscardUnknown(m)
}

var xxx_messageInfo_EmptyParams proto.InternalMessageInfo

type FilePath struct {
	Path                 string   `protobuf:"bytes,1,opt,name=path,proto3" json:"path,omitempty"`
	XXX_NoUnkeyedLiteral struct{} `json:"-"`
	XXX_unrecognized     []byte   `json:"-"`
	XXX_sizecache        int32    `json:"-"`
}

func (m *FilePath) Reset()         { *m = FilePath{} }
func (m *FilePath) String() string { return proto.CompactTextString(m) }
func (*FilePath) ProtoMessage()    {}
func (*FilePath) Descriptor() ([]byte, []int) {
	return fileDescriptor_common_32774cfc86ee87c3, []int{1}
}
func (m *FilePath) XXX_Unmarshal(b []byte) error {
	return xxx_messageInfo_FilePath.Unmarshal(m, b)
}
func (m *FilePath) XXX_Marshal(b []byte, deterministic bool) ([]byte, error) {
	return xxx_messageInfo_FilePath.Marshal(b, m, deterministic)
}
func (dst *FilePath) XXX_Merge(src proto.Message) {
	xxx_messageInfo_FilePath.Merge(dst, src)
}
func (m *FilePath) XXX_Size() int {
	return xxx_messageInfo_FilePath.Size(m)
}
func (m *FilePath) XXX_DiscardUnknown() {
	xxx_messageInfo_FilePath.DiscardUnknown(m)
}

var xxx_messageInfo_FilePath proto.InternalMessageInfo

func (m *FilePath) GetPath() string {
	if m != nil {
		return m.Path
	}
	return ""
}

type ResponseState struct {
	Status               ResponseStatus `protobuf:"varint,1,opt,name=status,proto3,enum=mgmt.ResponseStatus" json:"status,omitempty"`
	Error                string         `protobuf:"bytes,2,opt,name=error,proto3" json:"error,omitempty"`
	Info                 string         `protobuf:"bytes,3,opt,name=info,proto3" json:"info,omitempty"`
	XXX_NoUnkeyedLiteral struct{}       `json:"-"`
	XXX_unrecognized     []byte         `json:"-"`
	XXX_sizecache        int32          `json:"-"`
}

func (m *ResponseState) Reset()         { *m = ResponseState{} }
func (m *ResponseState) String() string { return proto.CompactTextString(m) }
func (*ResponseState) ProtoMessage()    {}
func (*ResponseState) Descriptor() ([]byte, []int) {
	return fileDescriptor_common_32774cfc86ee87c3, []int{2}
}
func (m *ResponseState) XXX_Unmarshal(b []byte) error {
	return xxx_messageInfo_ResponseState.Unmarshal(m, b)
}
func (m *ResponseState) XXX_Marshal(b []byte, deterministic bool) ([]byte, error) {
	return xxx_messageInfo_ResponseState.Marshal(b, m, deterministic)
}
func (dst *ResponseState) XXX_Merge(src proto.Message) {
	xxx_messageInfo_ResponseState.Merge(dst, src)
}
func (m *ResponseState) XXX_Size() int {
	return xxx_messageInfo_ResponseState.Size(m)
}
func (m *ResponseState) XXX_DiscardUnknown() {
	xxx_messageInfo_ResponseState.DiscardUnknown(m)
}

var xxx_messageInfo_ResponseState proto.InternalMessageInfo

func (m *ResponseState) GetStatus() ResponseStatus {
	if m != nil {
		return m.Status
	}
	return ResponseStatus_CTRL_SUCCESS
}

func (m *ResponseState) GetError() string {
	if m != nil {
		return m.Error
	}
	return ""
}

func (m *ResponseState) GetInfo() string {
	if m != nil {
		return m.Info
	}
	return ""
}

func init() {
	proto.RegisterType((*EmptyParams)(nil), "mgmt.EmptyParams")
	proto.RegisterType((*FilePath)(nil), "mgmt.FilePath")
	proto.RegisterType((*ResponseState)(nil), "mgmt.ResponseState")
	proto.RegisterEnum("mgmt.ResponseStatus", ResponseStatus_name, ResponseStatus_value)
}

func init() { proto.RegisterFile("common.proto", fileDescriptor_common_32774cfc86ee87c3) }

var fileDescriptor_common_32774cfc86ee87c3 = []byte{
	// 288 bytes of a gzipped FileDescriptorProto
	0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x6c, 0x91, 0x5f, 0x4b, 0xf3, 0x30,
	0x18, 0xc5, 0xdf, 0xee, 0x9d, 0x43, 0x1f, 0xb7, 0x11, 0x42, 0x2f, 0xaa, 0xa0, 0x48, 0xaf, 0x44,
	0xa4, 0x17, 0xfa, 0x09, 0x46, 0xe9, 0x46, 0x71, 0x4d, 0x43, 0xba, 0xb9, 0xcb, 0x12, 0xa5, 0x6e,
	0x03, 0xd3, 0x94, 0x26, 0xbb, 0xf0, 0x6b, 0x7b, 0xe9, 0x7f, 0x92, 0xd5, 0xcd, 0xa1, 0xb9, 0x7a,
	0x72, 0x7e, 0x27, 0xe7, 0x04, 0x1e, 0xe8, 0xde, 0x4b, 0x21, 0x64, 0x19, 0x54, 0xb5, 0xd4, 0x12,
	0xb7, 0xc5, 0x5c, 0x68, 0xbf, 0x07, 0x87, 0x91, 0xa8, 0xf4, 0x13, 0xe5, 0x35, 0x17, 0xca, 0x3f,
	0x85, 0xfd, 0xe1, 0xf2, 0xb1, 0xa0, 0x5c, 0x2f, 0x30, 0x86, 0x76, 0xc5, 0xf5, 0xc2, 0x73, 0xce,
	0x9c, 0xf3, 0x03, 0x66, 0x67, 0x7f, 0x0e, 0x3d, 0x56, 0xa8, 0x4a, 0x96, 0xaa, 0xc8, 0x34, 0xd7,
	0x05, 0xbe, 0x84, 0x8e, 0xd2, 0x5c, 0xaf, 0x94, 0xb5, 0xf5, 0xaf, 0xdc, 0xc0, 0xc4, 0x06, 0x3f,
	0x4d, 0x2b, 0xc5, 0x1a, 0x0f, 0x76, 0x61, 0xaf, 0xa8, 0x6b, 0x59, 0x7b, 0x2d, 0x9b, 0xb9, 0xbe,
	0x98, 0xa2, 0x65, 0xf9, 0x20, 0xbd, 0xff, 0xeb, 0x22, 0x33, 0x5f, 0x3c, 0x3b, 0xd0, 0xdf, 0x0d,
	0xc1, 0x08, 0xba, 0xe1, 0x84, 0x8d, 0xf3, 0x6c, 0x1a, 0x86, 0x51, 0x96, 0xa1, 0x7f, 0xd8, 0x05,
	0x64, 0x95, 0x98, 0xe4, 0x94, 0xa5, 0x23, 0x66, 0x54, 0x67, 0xe3, 0x9b, 0x0d, 0xe2, 0x49, 0x4c,
	0x46, 0xa8, 0x85, 0x8f, 0xa1, 0x67, 0x95, 0x88, 0xb1, 0x3c, 0x4c, 0xc9, 0x10, 0x7d, 0x7e, 0x1f,
	0x67, 0x87, 0x91, 0xdb, 0x24, 0x42, 0x1f, 0x5b, 0x76, 0xd4, 0x24, 0x19, 0x96, 0x85, 0x09, 0x7a,
	0xff, 0x1b, 0x0d, 0x28, 0x45, 0x6f, 0x5b, 0x74, 0xd2, 0xfc, 0xca, 0xa0, 0x29, 0xb9, 0x21, 0xe9,
	0x8c, 0xa0, 0xd7, 0xdf, 0x2f, 0x49, 0x9a, 0xc7, 0x09, 0x1d, 0xa3, 0x97, 0x0d, 0xba, 0xeb, 0xd8,
	0xcd, 0x5c, 0x7f, 0x05, 0x00, 0x00, 0xff, 0xff, 0xfe, 0x01, 0x8a, 0x86, 0xa9, 0x01, 0x00, 0x00,
}
