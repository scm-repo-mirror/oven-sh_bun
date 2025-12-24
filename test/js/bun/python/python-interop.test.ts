import { describe, expect, test } from "bun:test";
import { bunEnv, bunExe, tempDir } from "harness";

describe("Python imports", () => {
  test("import simple values from Python", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
count = 42
name = "hello"
pi = 3.14
flag = True
`,
      "test.js": `
import { count, name, pi, flag } from "./test.py";
console.log(JSON.stringify({ count, name, pi, flag }));
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(JSON.parse(stdout.trim())).toEqual({
      count: 42,
      name: "hello",
      pi: 3.14,
      flag: true,
    });
    expect(exitCode).toBe(0);
  });

  test("import and access dict properties", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
data = {
    'count': 1,
    'name': 'test'
}
`,
      "test.js": `
import { data } from "./test.py";
console.log(data.count);
console.log(data.name);
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("1\ntest");
    expect(exitCode).toBe(0);
  });

  test("modify dict from JS, visible in Python", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
data = {'count': 1}

def get_count():
    return data['count']

def get_new_key():
    return data.get('new_key', 'NOT SET')
`,
      "test.js": `
import { data, get_count, get_new_key } from "./test.py";

console.log("before:", get_count());
data.count = 999;
console.log("after:", get_count());

console.log("new_key before:", get_new_key());
data.new_key = "added from JS";
console.log("new_key after:", get_new_key());
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("before: 1\nafter: 999\nnew_key before: NOT SET\nnew_key after: added from JS");
    expect(exitCode).toBe(0);
  });

  test("nested object access and mutation", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
data = {
    'inner': {
        'value': 42
    }
}

def get_inner_x():
    return data['inner'].get('x', 'NOT SET')
`,
      "test.js": `
import { data, get_inner_x } from "./test.py";

const inner = data.inner;
console.log("inner.value:", inner.value);

console.log("before:", get_inner_x());
inner.x = "set from JS";
console.log("after:", get_inner_x());
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("inner.value: 42\nbefore: NOT SET\nafter: set from JS");
    expect(exitCode).toBe(0);
  });

  test("call Python functions with arguments", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
def add(a, b):
    return a + b

def greet(name):
    return f"Hello, {name}!"

def no_args():
    return "called with no args"
`,
      "test.js": `
import { add, greet, no_args } from "./test.py";

console.log(add(2, 3));
console.log(greet("World"));
console.log(no_args());
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("5\nHello, World!\ncalled with no args");
    expect(exitCode).toBe(0);
  });

  test("Python class instantiation and methods", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
class Counter:
    def __init__(self, start=0):
        self.value = start

    def increment(self):
        self.value += 1
        return self.value

    def get(self):
        return self.value
`,
      "test.js": `
import { Counter } from "./test.py";

const counter = Counter(10);
console.log("initial:", counter.get());
console.log("after increment:", counter.increment());
console.log("after increment:", counter.increment());
console.log("value property:", counter.value);
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("initial: 10\nafter increment: 11\nafter increment: 12\nvalue property: 12");
    expect(exitCode).toBe(0);
  });

  test("assign class instance to Python dict", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
class Potato:
    def __init__(self, name):
        self.name = name

    def greet(self):
        return f"I am {self.name}"

data = {}

def check():
    if 'item' in data:
        return f"name={data['item'].name}, greet={data['item'].greet()}"
    return "not found"
`,
      "test.js": `
import { Potato, data, check } from "./test.py";

console.log("before:", check());

const spud = Potato("Spudnik");
data.item = spud;

console.log("after:", check());
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("before: not found\nafter: name=Spudnik, greet=I am Spudnik");
    expect(exitCode).toBe(0);
  });

  test("Python lists", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
items = [1, 2, 3, "four", 5.0]

def get_length():
    return len(items)
`,
      "test.js": `
import { items, get_length } from "./test.py";

console.log("length:", get_length());
console.log("items[0]:", items[0]);
console.log("items[3]:", items[3]);
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("length: 5\nitems[0]: 1\nitems[3]: four");
    expect(exitCode).toBe(0);
  });

  test("None becomes null", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
nothing = None

def returns_none():
    return None
`,
      "test.js": `
import { nothing, returns_none } from "./test.py";

console.log("nothing:", nothing);
console.log("nothing === null:", nothing === null);
console.log("returns_none():", returns_none());
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("nothing: null\nnothing === null: true\nreturns_none(): null");
    expect(exitCode).toBe(0);
  });

  test("toString and console.log use Python str()", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
data = {'name': 'test', 'count': 42}

class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __str__(self):
        return f"Point({self.x}, {self.y})"
`,
      "test.js": `
import { data, Point } from "./test.py";

// toString() returns Python's str()
console.log(data.toString());

// String() coercion
console.log(String(data));

// Class with custom __str__
const p = Point(3, 4);
console.log(p.toString());

// console.log uses Python representation
console.log(data);
console.log(p);
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    const lines = stdout.trim().split("\n");
    // Dict toString
    expect(lines[0]).toBe("{'name': 'test', 'count': 42}");
    // Dict String()
    expect(lines[1]).toBe("{'name': 'test', 'count': 42}");
    // Point toString (custom __str__)
    expect(lines[2]).toBe("Point(3, 4)");
    // console.log dict
    expect(lines[3]).toBe("{'name': 'test', 'count': 42}");
    // console.log Point
    expect(lines[4]).toBe("Point(3, 4)");
    expect(exitCode).toBe(0);
  });

  test("Python print() output appears", async () => {
    using dir = tempDir("python-test", {
      "test.py": `
def say_hello(name):
    print(f"Hello, {name}!")
    return "done"

def multi_line():
    print("Line 1")
    print("Line 2")
`,
      "test.js": `
import { say_hello, multi_line } from "./test.py";

console.log("before");
say_hello("World");
console.log("middle");
multi_line();
console.log("after");
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.js"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("before\nHello, World!\nmiddle\nLine 1\nLine 2\nafter");
    expect(exitCode).toBe(0);
  });
});

describe("JavaScript imports in Python", () => {
  test("import simple values from JavaScript", async () => {
    using dir = tempDir("python-js-test", {
      "utils.js": `
export const count = 42;
export const name = "hello";
export const pi = 3.14;
export const flag = true;
`,
      "test.py": `
import utils

print(utils.count)
print(utils.name)
print(utils.pi)
print(utils.flag)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("42\nhello\n3.14\nTrue");
    expect(exitCode).toBe(0);
  });

  test("call JavaScript functions from Python", async () => {
    using dir = tempDir("python-js-test", {
      "math.js": `
export function add(a, b) {
  return a + b;
}

export function greet(name) {
  return "Hello, " + name + "!";
}

export function noArgs() {
  return "called with no args";
}
`,
      "test.py": `
import math

print(math.add(2, 3))
print(math.greet("Python"))
print(math.noArgs())
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("5\nHello, Python!\ncalled with no args");
    expect(exitCode).toBe(0);
  });

  test("access JavaScript object properties", async () => {
    using dir = tempDir("python-js-test", {
      "config.js": `
export const config = {
  name: "MyApp",
  version: "1.0.0",
  settings: {
    debug: true,
    port: 3000
  }
};
`,
      "test.py": `
import config

print(config.config.name)
print(config.config.version)
print(config.config.settings.debug)
print(config.config.settings.port)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("MyApp\n1.0.0\nTrue\n3000");
    expect(exitCode).toBe(0);
  });

  test("subscript access on JavaScript objects", async () => {
    using dir = tempDir("python-js-test", {
      "data.js": `
export const obj = { count: 1, name: "test" };
export const arr = [10, 20, 30];
`,
      "test.py": `
import data

print(data.obj['count'])
print(data.obj['name'])
print(data.arr[0])
print(data.arr[2])
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("1\ntest\n10\n30");
    expect(exitCode).toBe(0);
  });

  test("modify JavaScript objects from Python", async () => {
    using dir = tempDir("python-js-test", {
      "state.js": `
export const obj = { count: 1 };

export function getCount() {
  return obj.count;
}
`,
      "test.py": `
import state

print(state.getCount())
state.obj['count'] = 999
print(state.getCount())
state.obj.count = 42
print(state.getCount())
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("1\n999\n42");
    expect(exitCode).toBe(0);
  });

  test("import TypeScript from Python", async () => {
    using dir = tempDir("python-ts-test", {
      "utils.ts": `
export function multiply(a: number, b: number): number {
  return a * b;
}

export const PI: number = 3.14159;

interface Config {
  name: string;
}

export const config: Config = { name: "TypeScript" };
`,
      "test.py": `
import utils

print(utils.multiply(6, 7))
print(utils.PI)
print(utils.config.name)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("42\n3.14159\nTypeScript");
    expect(exitCode).toBe(0);
  });

  test("bidirectional: Python calls JS which calls Python", async () => {
    using dir = tempDir("python-bidirectional", {
      "helper.py": `
def double(x):
    return x * 2

def format_result(value):
    return f"Result: {value}"
`,
      "processor.js": `
import { double, format_result } from "./helper.py";

export function process(value) {
  const doubled = double(value);
  return format_result(doubled);
}
`,
      "main.py": `
import processor

result = processor.process(21)
print(result)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "main.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("Result: 42");
    expect(exitCode).toBe(0);
  });

  test("JavaScript undefined and null become None", async () => {
    using dir = tempDir("python-js-null", {
      "nulls.js": `
export const nothing = null;
export const undef = undefined;

export function returnsNull() {
  return null;
}

export function returnsUndefined() {
  return undefined;
}
`,
      "test.py": `
import nulls

print(nulls.nothing)
print(nulls.undef)
print(nulls.returnsNull())
print(nulls.returnsUndefined())
print(nulls.nothing is None)
print(nulls.undef is None)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    expect(stdout.trim()).toBe("None\nNone\nNone\nNone\nTrue\nTrue");
    expect(exitCode).toBe(0);
  });

  test("multiple imports of same module use cached version", async () => {
    using dir = tempDir("python-multi-import", {
      "counter.js": `
export let count = 0;

export function increment() {
  count++;
  return count;
}
`,
      "test.py": `
import counter
import counter as counter2

# Both should refer to the same module
print(counter.increment())
print(counter2.increment())
print(counter.count)
print(counter2.count)
print(counter is counter2)
`,
    });

    await using proc = Bun.spawn({
      cmd: [bunExe(), "test.py"],
      cwd: String(dir),
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });

    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);

    // Both imports should share state - count increments from 1 to 2
    expect(stdout.trim()).toBe("1\n2\n2\n2\nTrue");
    expect(exitCode).toBe(0);
  });
});
